#include "webui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "camera_res.h"
#include "cfg.h"
#include "ctrl_server.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "health.h"
#include "http_origin.h"
#include "lpc_link.h"
#include "mbedtls/platform_util.h"
#include "machine.h"
#include "ota_httpd.h"
#include "play.h"
#include "sdlog.h"
#include "storage.h"
#include "webui_fmt.h"
#include "wifi_mgr.h"

static const char *TAG = "webui";

#define LOG_LINES 100      // GET /api/log default
#define LOG_LINES_MAX 400  // more than the RAM ring holds
#define LOG_BUF (15 * 1024)  // with the status, /api/diag stays near 16 KB

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

static bool (*s_lpc_silent)(void);
static uint64_t s_log_base;  // random per boot, added to the log cursor: one from before a restart misses
static char s_other_version[33];  // the other slot's app, read once: it changes only by an OTA, which restarts

// Only the httpd task uses these.
static char s_report[MACHINE_REPORT_MAX];
static char s_json[2048];

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software (update or command)";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    default: return "unknown";
    }
}

// The status JSON into s_json; returns its length.
static size_t status_json(void)
{
    char state[MACHINE_STATE_MAX] = "";
    bool fresh = machine_get_report(s_report, sizeof(s_report)) && machine_get_state(state, sizeof(state));
    if (!fresh) {
        s_report[0] = '\0';
    }
    char lpc_ver[16], lpc_cfg[64], job[160];
    ctrl_server_lpc_version(lpc_ver, sizeof(lpc_ver));
    cfg_lpc_status(lpc_cfg, sizeof(lpc_cfg));
    play_job_file(job, sizeof(job));
    uint32_t used, total;
    storage_space_mb(&used, &total);
    wifi_mgr_status_t wifi;
    wifi_mgr_get_status(&wifi);  // its SSID never goes into the page
    const esp_partition_t *run = esp_ota_get_running_partition(), *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t img;
    bool pending = esp_ota_get_state_partition(run, &img) == ESP_OK && img == ESP_OTA_IMG_PENDING_VERIFY;
    webui_status_t s = {
        .fresh = fresh,
        .state = state,
        .report = s_report,
        .lpc_silent = s_lpc_silent && s_lpc_silent(),
        .lpc_rate_x10 = machine_status_rate_x10(),
        .lpc_version = lpc_ver,
        .lpc_config = lpc_cfg,
        .job_file = job,
        .job_serving = play_job_serving(),
        .sd_mounted = storage_mounted(),
        .sd_used_mb = used,
        .sd_total_mb = total,
        .cam_res = camera_res(),
        .wifi_connected = wifi.connected,
        .rssi = wifi.connected ? wifi.rssi : 0,
        .ip = wifi.connected ? wifi.ip : "",
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
        .reset_reason = reset_reason(),
        .version = esp_app_get_description()->version,
        .slot = run->label,
        .slot_state = pending ? "pending" : "valid",
        .other_slot = other ? other->label : "",
        .other_version = s_other_version,
        .heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        .psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        .psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
        .cpu = {health_cpu_load(0), health_cpu_load(1)},
    };
    return webui_status_json(&s, s_json, sizeof(s_json));
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, text);
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");  // no framing by other pages (clickjacking)
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (wifi_mgr_on_setup_ap(httpd_req_to_sockfd(req))) {  // the hotspot gets the Wi-Fi setup page only
        return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start - 1);
    }
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);  // without the NUL
}

static esp_err_t refused(httpd_req_t *req)
{
    return reply(req, "403 Forbidden", "cross-site request refused\n");
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!ota_host_ok(req)) {
        return refused(req);
    }
    size_t n = status_json();
    if (n == 0) {
        return reply(req, "500 Internal Server Error", "status does not fit\n");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_json, n);
}

// The log with every address but the board's own masked: the last `lines`
// lines, or with since, the lines after *cursor (sdlog_since). *cursor: where
// the next read goes on. NULL if out of memory.
static char *log_read(unsigned lines, bool since, uint64_t *cursor, bool *gap)
{
    char *buf = heap_caps_malloc_prefer(LOG_BUF, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (!buf) {
        return NULL;
    }
    *gap = false;
    if (since) {
        sdlog_since(cursor, buf, LOG_BUF, gap);
    } else {
        sdlog_tail(buf, LOG_BUF, lines, cursor);
    }
    wifi_mgr_status_t wifi;
    wifi_mgr_get_status(&wifi);
    webui_redact_ips(buf, wifi.connected ? wifi.ip : "");
    return buf;
}

// ?n= lines of the tail, or ?since=<cursor> what came after it. The cursor
// for the next poll is in the X-Log-Next header; X-Log-Gap: 1 when lines
// were left out in between.
static esp_err_t log_get(httpd_req_t *req)
{
    if (!ota_host_ok(req)) {
        return refused(req);
    }
    unsigned lines = LOG_LINES;
    uint64_t cursor = 0;
    bool since = false, gap;
    char query[48], v[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "n", v, sizeof(v)) == ESP_OK) {
            lines = (unsigned)strtoul(v, NULL, 10);
            lines = lines < 1 ? 1 : lines > LOG_LINES_MAX ? LOG_LINES_MAX : lines;
        }
        since = httpd_query_key_value(query, "since", v, sizeof(v)) == ESP_OK && webui_parse_u64(v, &cursor);
        if (since) {
            cursor = cursor >= s_log_base ? cursor - s_log_base : UINT64_MAX;  // past the end: a gap
        }
    }
    char *buf = log_read(lines, since, &cursor, &gap);
    if (!buf) {
        return reply(req, "503 Service Unavailable", "out of memory\n");
    }
    char next[24];
    snprintf(next, sizeof(next), "%llu", (unsigned long long)(cursor + s_log_base));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Log-Next", next);
    if (gap) {
        httpd_resp_set_hdr(req, "X-Log-Gap", "1");
    }
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

// Status, versions and the log, for a bug report: no SSID, password, serial
// number or client address.
static esp_err_t diag_get(httpd_req_t *req)
{
    if (!ota_host_ok(req)) {
        return refused(req);
    }
    uint64_t cursor;
    bool gap;
    char *buf = log_read(LOG_LINES_MAX, false, &cursor, &gap);
    if (!buf) {
        return reply(req, "503 Service Unavailable", "out of memory\n");
    }
    const esp_app_desc_t *app = esp_app_get_description();
    char sha[CONFIG_APP_RETRIEVE_LEN_ELF_SHA + 1];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    char head[384];
    snprintf(head, sizeof(head),
             "Z1 Mainboard diagnostics (open firmware)\n"
             "firmware: %s %s, ELF %s, IDF %s\n\nstatus: ",  // no date: reproducible build
             app->project_name, app->version, sha, app->idf_ver);
    status_json();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"z1-diagnostics.txt\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr_chunk(req, head);
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, s_json);
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, "\n\nlog (newest last):\n");
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, buf);
    }
    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, NULL);
    }
    free(buf);
    return err;
}

// The page's own requests only. A page on another site can make the browser
// POST here, but it cannot add a custom header without a CORS preflight,
// which this server never grants (it has no OPTIONS handler), and its Origin
// is not ours.
static bool from_our_page(httpd_req_t *req)
{
    return httpd_req_get_hdr_value_len(req, "X-Requested-With") > 0 && ota_same_origin(req);
}

// The app's own realtime bytes, sent as the app server forwards them.
static esp_err_t send_realtime(httpd_req_t *req, uint8_t byte, const char *what)
{
    if (!from_our_page(req)) {
        return refused(req);
    }
    esp_err_t err = lpc_link_send(PTYPE_CTRL_SINGLE, &byte, 1);
    ESP_LOGW(TAG, "%s from the web UI: %s", what, err == ESP_OK ? "sent to the LPC" : esp_err_to_name(err));
    if (err != ESP_OK) {
        return reply(req, "503 Service Unavailable", "motion controller link not running\n");
    }
    char text[64];
    snprintf(text, sizeof(text), "%s sent\n", what);
    return reply(req, "200 OK", text);
}

// Stop: the halt byte 0x18, as the app's Stop during a job. The LPC stops the
// steppers at once and flushes its queue, switches the spindle off, aborts the
// job (PLAY_CAN) and goes to Alarm ($X or homing to go on). The app's `abort` (sent when the job is paused) would
// first run every queued move to its end (Player.cpp abort_command).
static esp_err_t stop_post(httpd_req_t *req)
{
    return send_realtime(req, 0x18, "stop (halt)");
}

// Pause: feed hold; the machine decelerates and holds, resumable from the app.
static esp_err_t pause_post(httpd_req_t *req)
{
    return send_realtime(req, '!', "pause (feed hold)");
}

// One current camera picture (camera_snapshot); 404 without a camera.
static esp_err_t snapshot_get(httpd_req_t *req)
{
    if (!ota_host_ok(req)) {
        return refused(req);
    }
    uint8_t *jpeg;
    size_t len;
    esp_err_t err = camera_snapshot(&jpeg, &len);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return reply(req, "404 Not Found", "no camera on this board\n");
    }
    if (err != ESP_OK) {
        return reply(req, "503 Service Unavailable",
                     err == ESP_ERR_INVALID_STATE ? "camera busy, try again\n" : "no picture from the camera\n");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, (const char *)jpeg, len);
    free(jpeg);
    return err;
}

// The camera resolution: the body is one of camera_res_names. Applied at once
// (between two frames) and kept for the next boots.
static esp_err_t camera_post(httpd_req_t *req)
{
    if (!from_our_page(req)) {
        return refused(req);
    }
    char body[16];
    int n = req->content_len < sizeof(body) ? httpd_req_recv(req, body, req->content_len) : -1;
    int res = n >= 0 && (size_t)n == req->content_len ? camera_res_parse(body, (size_t)n) : -1;
    if (res < 0) {
        return reply(req, "400 Bad Request", "unknown resolution\n");
    }
    esp_err_t err = camera_set_res(res);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return reply(req, "404 Not Found", "no camera on this board\n");
    }
    ESP_LOGI(TAG, "camera resolution %s from the web UI", camera_res_names[res]);
    return reply(req, "200 OK", err == ESP_OK ? "resolution set\n" : "resolution set, but not saved for the next boot\n");
}

// ---- the setup hotspot's page (wifi_mgr.h): scan and join through the
// app's `wlan` path (wifi_mgr_app_command: saved only once it gives an IP),
// run in wifi_mgr's task; the page polls /setup/result.

static SemaphoreHandle_t s_setup_lock;
static struct {
    bool started, busy, ok;
    char text[24 * 48];  // a scan's lines
} s_setup;

// A client of the setup hotspot, with an IPv4 Host (no rebound names) and,
// for a POST, our page's header and Origin.
static bool setup_client(httpd_req_t *req, bool post)
{
    char host[64], origin[80];
    if (!wifi_mgr_on_setup_ap(httpd_req_to_sockfd(req)) ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || !http_host_ok(host, "")) {
        return false;
    }
    if (!post) {
        return true;
    }
    esp_err_t o = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    return httpd_req_get_hdr_value_len(req, "X-Requested-With") > 0 &&
           (o == ESP_ERR_NOT_FOUND || (o == ESP_OK && http_origin_ok(origin, host)));
}

static void setup_done(void *ctx, const char *text, bool ok)
{
    xSemaphoreTake(s_setup_lock, portMAX_DELAY);
    snprintf(s_setup.text, sizeof(s_setup.text), "%s", text);
    s_setup.ok = ok;
    s_setup.busy = false;
    xSemaphoreGive(s_setup_lock);
}

static esp_err_t setup_run(httpd_req_t *req, wifi_app_cmd_t cmd, const char *ssid, const char *pass)
{
    xSemaphoreTake(s_setup_lock, portMAX_DELAY);
    bool go = !s_setup.busy;
    if (go) {
        s_setup.started = s_setup.busy = true;
        s_setup.text[0] = '\0';
    }
    xSemaphoreGive(s_setup_lock);
    if (go && !wifi_mgr_app_command(cmd, ssid, pass, setup_done, NULL)) {
        setup_done(NULL, "", false);
        go = false;
    }
    return go ? reply(req, "202 Accepted", "started\n") : reply(req, "409 Conflict", "busy, try again\n");
}

static esp_err_t setup_scan_post(httpd_req_t *req)
{
    if (!setup_client(req, true)) {
        return reply(req, "403 Forbidden", "only on the setup hotspot\n");
    }
    return setup_run(req, WIFI_APP_SCAN, NULL, NULL);
}

// Body: "<ssid>\n<password>". The password is never logged and wiped here;
// wifi_mgr wipes its copy after the attempt.
static esp_err_t setup_join_post(httpd_req_t *req)
{
    if (!setup_client(req, true)) {
        return reply(req, "403 Forbidden", "only on the setup hotspot\n");
    }
    char body[WIFI_SSID_MAX + WIFI_PASSWORD_MAX + 3];
    size_t got = 0;
    esp_err_t err = ESP_OK;
    if (req->content_len >= sizeof(body)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    while (err == ESP_OK && got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            err = ESP_FAIL;
        } else {
            got += (size_t)n;
        }
    }
    body[got] = '\0';
    char *nl = strchr(body, '\n');
    if (nl) {
        *nl = '\0';
    }
    if (err == ESP_OK && nl && wifi_policy_creds_valid(body, nl + 1)) {
        err = setup_run(req, WIFI_APP_JOIN, body, nl + 1);
    } else {
        err = reply(req, "400 Bad Request", "network name 1-32 bytes; password empty or 8-63 characters\n");
    }
    mbedtls_platform_zeroize(body, sizeof(body));
    return err;
}

// "none" | "busy" | "ok" | "error", a newline, then the reply text (a scan:
// the app's lines "<ssid>,<auth>,<rssi>,<connected>", spaces as 0x01).
static esp_err_t setup_result_get(httpd_req_t *req)
{
    if (!setup_client(req, false)) {
        return reply(req, "403 Forbidden", "only on the setup hotspot\n");
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    xSemaphoreTake(s_setup_lock, portMAX_DELAY);
    const char *state = !s_setup.started ? "none\n" : s_setup.busy ? "busy\n" : s_setup.ok ? "ok\n" : "error\n";
    esp_err_t err = httpd_resp_sendstr_chunk(req, state);
    if (err == ESP_OK && s_setup.started && !s_setup.busy) {
        err = httpd_resp_sendstr_chunk(req, s_setup.text);
    }
    xSemaphoreGive(s_setup_lock);
    return err == ESP_OK ? httpd_resp_sendstr_chunk(req, NULL) : err;
}

esp_err_t webui_start(bool (*lpc_silent)(void))
{
    s_setup_lock = xSemaphoreCreateMutex();
    if (!s_setup_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_lpc_silent = lpc_silent;
    s_log_base = (uint64_t)esp_random() << 24;
    httpd_handle_t h = ota_httpd();
    if (!h) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t desc;
    if (other && esp_ota_get_partition_description(other, &desc) == ESP_OK) {
        snprintf(s_other_version, sizeof(s_other_version), "%s", desc.version);
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/log", .method = HTTP_GET, .handler = log_get},
        {.uri = "/api/diag", .method = HTTP_GET, .handler = diag_get},
        {.uri = "/api/stop", .method = HTTP_POST, .handler = stop_post},
        {.uri = "/api/pause", .method = HTTP_POST, .handler = pause_post},
        {.uri = "/api/snapshot", .method = HTTP_GET, .handler = snapshot_get},
        {.uri = "/api/camera", .method = HTTP_POST, .handler = camera_post},
        {.uri = "/setup/scan", .method = HTTP_POST, .handler = setup_scan_post},
        {.uri = "/setup/join", .method = HTTP_POST, .handler = setup_join_post},
        {.uri = "/setup/result", .method = HTTP_GET, .handler = setup_result_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(h, &uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    ESP_LOGI(TAG, "web UI on http://<ip>/");
    return ESP_OK;
}
