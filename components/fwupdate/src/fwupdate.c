// Firmware updates for the web UI: the latest GitHub release of either chip,
// fetched by the board itself, and the motion controller's file upload.
//
//   GET  /api/update   {"esp":{..},"lpc":{..}}, each
//                      {"state","current","latest","src","beta","pct","msg"}
//   POST /api/update   body "<check|install> <esp|lpc>[ beta][ community]"
//   POST /api/lpcfw    body = lpc1768.bin, or Makera's MakeraZ1-*-bin.bin package
//
// state: idle, checking, current (no newer release; it can be reinstalled),
// available, checkfail (the check did not work), downloading, installing (the
// motion controller takes it), done, failed (an install), restarting (the
// Mainboard restarts into the new image). beta takes the newest release
// including pre-releases, else the latest stable one.
// community (lpc only) checks the Carvera Community firmware's Z1 image
// instead of Makera's; switching between the two always counts as newer.
//
// The Mainboard image goes through the same checks as POST /ota (signature,
// board, not older, no job); the motion controller's file through lpcfw's
// (bundle CRCs or the LPC1768 vector table, the machine Idle, no job). The
// board talks to GitHub only on these requests.

#include "fwupdate.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "ctrl_server.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lpcfw.h"
#include "lpcfw_bundle.h"
#include "ota.h"
#include "ota_httpd.h"
#include "ota_install.h"
#include "ota_version.h"
#include "storage.h"
#include "xfer_fs.h"

static const char *TAG = "fwupdate";

#define TASK_STACK 12288           // TLS, then the RSA-3072 check in esp_ota_end
#define STALL_US (60 * 1000000LL)  // no new data this long: give up
#define BODY_MAX 24576             // of the release JSON: the tag and the assets come before the notes
#define LPC_TMP "/sd/.firmware.bin.part"
#define UPLOAD_MAX (4u << 20)      // Makera's package (with its ESP32 image) is about 2 MB
#define LPC_WAIT_MS (5 * 60 * 1000)  // the motion controller's install takes about a minute
#define RECV_RETRIES 7

typedef struct {
    const char *id;               // "open-z1", "makera", "community"
    const char *repo;             // owner/name on GitHub
    const char *prefix, *suffix;  // the release asset's file name
} source_t;

static const source_t ESP_SRC = {"open-z1", CONFIG_Z1_UPDATE_REPO, "open-z1-", "-z1-ota.bin"};
static const source_t MAKERA_SRC = {"makera", "MakeraInc/MakeraZ1Firmware", "lpc1768", ".bin"};
static const source_t COMMUNITY_SRC = {"community", "Carvera-Community/Carvera_Community_Firmware", "firmware-z1-", ".bin"};

typedef struct {
    const char *name;  // "esp", "lpc"
    // Guarded by s_lock:
    const char *src;   // the source_t id of latest and url
    bool beta;         // and whether that check took pre-releases
    const char *state;
    char latest[24];  // the release's tag
    char url[320];    // its asset
    int pct;
    char msg[112];
} target_t;

enum { ESP, LPC };
static target_t s_t[2] = {
    [ESP] = {.name = "esp", .src = "", .state = "idle"},
    [LPC] = {.name = "lpc", .src = "", .state = "idle"},
};

static SemaphoreHandle_t s_lock;
static atomic_bool s_running;  // a task exists (one at a time)
static uint8_t s_buf[4096];    // the task's, or the httpd task's under s_running

typedef enum { DO_CHECK, DO_INSTALL, DO_WATCH } action_t;
static bool s_beta;                // for DO_CHECK, set with s_running taken
static const source_t *s_src = &ESP_SRC;

static void set(target_t *t, const char *state, int pct, const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    t->state = state;
    t->pct = pct;
    snprintf(t->msg, sizeof(t->msg), "%s", msg ? msg : "");
    xSemaphoreGive(s_lock);
}

// lpcfw's texts are for the app: "Error: ...\r\n" -> "..." (in a static
// buffer: callers hold s_running).
static const char *lpcfw_text(const char *text)
{
    static char m[112];
    if (!strncmp(text, "Error: ", 7)) {
        text += 7;
    }
    snprintf(m, sizeof(m), "%.*s", (int)strcspn(text, "\r\n"), text);
    size_t n = strlen(m);
    if (n && m[n - 1] == '.') {
        m[n - 1] = '\0';  // the page adds its own
    }
    return m;
}

static void running_version(int target, char *out, size_t cap)
{
    if (target == ESP) {
        snprintf(out, cap, "%s", esp_app_get_description()->version);
    } else {
        ctrl_server_lpc_build(out, cap);
    }
}

static const char *version_of(const char *tag)
{
    return tag[0] == 'v' ? tag + 1 : tag;
}

// The quoted string after "key": copied into out if it has only chars from ok.
static const char *json_str(const char *p, const char *key, char *out, size_t n, const char *ok)
{
    char k[32];
    snprintf(k, sizeof(k), "\"%s\"", key);
    if (!(p = strstr(p, k))) {
        return NULL;
    }
    p += strlen(k);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p++ != '"') {
        return NULL;
    }
    size_t i = 0;
    for (; *p && *p != '"'; p++) {
        if (i + 1 >= n || !(isalnum((unsigned char)*p) || strchr(ok, *p))) {
            return NULL;  // it goes into a URL and into JSON: plain names only
        }
        out[i++] = *p;
    }
    out[i] = '\0';
    return *p == '"' && i > 0 ? p : NULL;
}

// A community build's version has a "c" right after its number, at the end
// or before a "-" ("2.3.0c", "2.3.0c-RC2", "2026.11.18c"); Makera's are plain
// numbers. (The page's isCommunity is the same rule.)
static bool community_version(const char *v)
{
    for (const char *c = strchr(v, 'c'); c; c = strchr(c + 1, 'c')) {
        if (c > v && isdigit((unsigned char)c[-1]) && (c[1] == '\0' || c[1] == '-')) {
            return true;
        }
    }
    return false;
}

// Asks GitHub for src's latest (or newest, beta) release: the URL of its
// asset prefix*suffix and its version (from the file name if it has one,
// else the tag).
static const char *check(const source_t *src, bool beta, char *ver, size_t vn, char *url, size_t un)
{
    char api[160], tag[32];
    snprintf(api, sizeof(api), "https://api.github.com/repos/%s/releases%s", src->repo, beta ? "?per_page=1" : "/latest");
    esp_http_client_config_t cfg = {
        .url = api,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .user_agent = "open-z1",
        .buffer_size = 2048,
    };
    char *body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM);
    esp_http_client_handle_t h = body ? esp_http_client_init(&cfg) : NULL;
    if (!h) {
        free(body);
        return "out of memory";
    }
    esp_http_client_set_header(h, "Accept", "application/vnd.github+json");
    const char *why = NULL;
    int status = 0;
    if (esp_http_client_open(h, 0) != ESP_OK || esp_http_client_fetch_headers(h) < 0) {
        why = "cannot reach GitHub (no internet?)";
    } else if ((status = esp_http_client_get_status_code(h)) == 404) {
        why = "no release found";
    } else if (status != 200) {
        ESP_LOGW(TAG, "GitHub answered %d", status);
        why = "GitHub refused the request; try again later";
    } else {
        int len = 0, r;
        while (len < BODY_MAX - 1 && (r = esp_http_client_read(h, body + len, BODY_MAX - 1 - len)) > 0) {
            len += r;
        }
        body[len] = '\0';
        char prefix[200];
        snprintf(prefix, sizeof(prefix), "https://github.com/%s/releases/download/", src->repo);
        if (!json_str(body, "tag_name", tag, sizeof(tag), ".-_")) {
            why = "no release found";
        } else {
            why = "the release has no file for this board";
            const char *key = "\"browser_download_url\"";
            for (const char *p = strstr(body, key); p; p = strstr(p + strlen(key), key)) {
                if (!json_str(p, "browser_download_url", url, un, ".-_:/%+~")) {
                    continue;  // one odd asset hides no other
                }
                const char *name = strrchr(url, '/');
                name = name ? name + 1 : url;
                size_t nl = strlen(name), pl = strlen(src->prefix), sl = strlen(src->suffix);
                if (!strncmp(url, prefix, strlen(prefix)) && !strncmp(name, src->prefix, pl) && nl >= pl + sl &&
                    !strcmp(name + nl - sl, src->suffix) && !strstr(name, "debug")) {
                    // open-z1-1.0.0-z1-ota.bin, firmware-z1-2.3.0c.bin: the
                    // version is in the name; lpc1768.bin: only in the tag.
                    if (src->prefix[pl - 1] == '-' && nl > pl + sl) {
                        snprintf(ver, vn, "%.*s", (int)(nl - pl - sl), name + pl);
                    } else {
                        snprintf(ver, vn, "%s", tag);
                    }
                    why = NULL;
                    break;
                }
            }
        }
    }
    esp_http_client_cleanup(h);
    free(body);
    return why;
}

static const char *install_esp(const char *url, const char **state)
{
    *state = "failed";
    const char *why = ota_claim();
    if (why) {
        return why;
    }
    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "open-z1",
        .buffer_size = 4096,     // GitHub redirects to a long asset URL
        .buffer_size_tx = 4096,  // which then goes into the request line
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    ESP_LOGI(TAG, "downloading %s", url);
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "download: %s", esp_err_to_name(err));
        ota_release();
        return "download failed (no internet?)";
    }
    const char *status;
    esp_app_desc_t desc;
    why = esp_https_ota_get_img_desc(h, &desc) != ESP_OK ? "not an ESP32-S3 app image"
                                                         : ota_check_desc(&desc, false, &status);
    int size = esp_https_ota_get_image_size(h), last = -1, got = -1;
    int64_t moved = esp_timer_get_time();
    while (!why && (err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int now = esp_https_ota_get_image_len_read(h);
        if (now != got) {
            got = now;
            moved = esp_timer_get_time();
        } else if (esp_timer_get_time() - moved > STALL_US) {
            why = "download stalled";
            break;
        }
        int pct = size > 0 ? (int)(100LL * now / size) : 0;
        if (pct != last) {
            set(&s_t[ESP], "downloading", pct, NULL);
            if (pct / 10 != last / 10) {
                ESP_LOGI(TAG, "download %d %%", pct);
            }
            last = pct;
        }
    }
    if (!why && (err != ESP_OK || !esp_https_ota_is_complete_data_received(h))) {
        ESP_LOGW(TAG, "download: %s", esp_err_to_name(err));
        why = "download interrupted";
    }
    if (!why) {
        why = ota_busy_now();
    }
    if (why) {
        esp_https_ota_abort(h);
        ota_release();
        return why;
    }
    err = esp_https_ota_finish(h);  // checks the image and its signature, selects it
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "install: %s", esp_err_to_name(err));
        ota_release();
        return err == ESP_ERR_OTA_VALIDATE_FAILED ? "image damaged or not signed with this firmware's key"
                                                  : "image check failed";
    }
    *state = "restarting";
    return NULL;
}

// Why the motion controller's file cannot be taken now, or NULL.
static const char *lpc_refused(void)
{
    const char *why = lpcfw_can_upgrade();
    if (why) {
        return lpcfw_text(why);
    }
    if (ctrl_server_uploading()) {
        return "an upload from the app is running";
    }
    if (storage_read_only()) {
        return "the SD card is read-only";
    }
    return NULL;
}

// LPC_TMP is complete: puts it in place as /sd/firmware.bin and hands it to
// lpcfw (as the app's `upgrade` does). Call under storage_hold.
static const char *lpc_start(void)
{
    if (!xfer_fs_keep(LPC_TMP, LPCFW_BUNDLE, NULL)) {
        remove(LPC_TMP);
        return "cannot write the SD card";
    }
    const char *why = lpcfw_upgrade();
    return why ? lpcfw_text(why) : NULL;
}

static const char *download_lpc(target_t *t, const char *url)
{
    const char *why = lpc_refused();
    if (why) {
        return why;
    }
    if (!storage_hold()) {
        return "no SD card";
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "open-z1",
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    FILE *f = h ? fopen(LPC_TMP, "wb") : NULL;
    int64_t len = -1;
    if (!f) {
        why = h ? "cannot write the SD card" : "out of memory";
    } else {
        ESP_LOGI(TAG, "downloading %s", url);
        for (int redirects = 0;; redirects++) {
            if (esp_http_client_open(h, 0) != ESP_OK || (len = esp_http_client_fetch_headers(h)) < 0) {
                why = "download failed (no internet?)";
                break;
            }
            int status = esp_http_client_get_status_code(h);
            bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
            if (redirect && redirects < 5) {
                esp_http_client_flush_response(h, NULL);
                esp_http_client_set_redirection(h);
                esp_http_client_close(h);
                continue;
            }
            if (status != 200) {
                ESP_LOGW(TAG, "download: HTTP %d", status);
                why = "download failed";
            } else if (len <= 0 || len > LPCFW_LPC_MAX) {
                why = "the release file has an unexpected size";
            }
            break;
        }
    }
    int64_t got = 0, moved = esp_timer_get_time();
    while (!why && got < len) {
        int r = esp_http_client_read(h, (char *)s_buf, sizeof(s_buf));
        if (r == -ESP_ERR_HTTP_EAGAIN) {  // a read timeout: it blocked timeout_ms
            if (esp_timer_get_time() - moved > STALL_US) {
                why = "download stalled";
            }
            continue;
        }
        if (r <= 0) {  // 0 before the end: the connection closed
            why = "download interrupted";
        } else if (fwrite(s_buf, 1, r, f) != (size_t)r) {
            why = "cannot write the SD card";
        } else {
            got += r;
            moved = esp_timer_get_time();
            set(t, "downloading", (int)(100 * got / len), NULL);
        }
    }
    if (!why && got != len) {
        why = "download interrupted";
    }
    if (f && (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0) && !why) {
        why = "cannot write the SD card";
    }
    if (h) {
        esp_http_client_cleanup(h);
    }
    if (!why) {
        why = lpc_start();
    } else {
        remove(LPC_TMP);
    }
    storage_release();
    return why;
}

// Follows lpcfw (|OTA:phase,pct) until the motion controller runs again.
static void watch_lpc(target_t *t)
{
    set(t, "installing", 0, NULL);
    unsigned phase = 0, pct = 0;
    int idle_ms = 0;
    bool ours = false;  // this attempt's transfer was seen: a done or failed shown
                        // before is an earlier attempt's (lpcfw holds it 3 or 10 s)
    for (int ms = 0; ms < LPC_WAIT_MS; ms += 250) {
        vTaskDelay(pdMS_TO_TICKS(250));
        lpcfw_ota_report(&phase, &pct);
        ours |= phase == 2;
        if (ours && phase == 4) {
            set(t, "done", 100, NULL);
            return;
        }
        if (ours && phase == 3) {
            set(t, "failed", 0, "the motion controller did not take it; see the log");
            return;
        }
        if (phase == 2) {
            set(t, "installing", (int)pct, NULL);
        }
        // Nothing running any more and no transfer seen: lpcfw ended on its
        // own (a bad file, the bootloader did not ask); its texts are in the log.
        idle_ms = !lpcfw_busy() && (!ours || phase == 0) ? idle_ms + 250 : 0;
        if (idle_ms >= 3000) {
            set(t, ours ? "done" : "failed", ours ? 100 : 0,
                ours ? NULL : "the motion controller did not start its update; see the log");
            return;
        }
    }
    set(t, "failed", 0, "no answer from the motion controller; see the log");
}

static void task(void *arg)
{
    int target = (intptr_t)arg & 1;
    action_t act = (action_t)((intptr_t)arg >> 1);
    target_t *t = &s_t[target];
    char tag[sizeof(t->latest)], url[sizeof(t->url)];
    if (act == DO_CHECK) {
        const char *why = check(s_src, s_beta, tag, sizeof(tag), url, sizeof(url));
        if (why) {
            ESP_LOGW(TAG, "%s update check: %s", t->name, why);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            t->src = s_src->id;  // what this check was for (the page compares)
            t->beta = s_beta;
            xSemaphoreGive(s_lock);
            set(t, "checkfail", 0, why);
        } else {
            char running[24];
            running_version(target, running, sizeof(running));
            bool other_family = target == LPC && community_version(tag) != community_version(running);
            bool newer = !running[0] || other_family || ota_version_cmp(version_of(tag), running) > 0;
            ESP_LOGI(TAG, "%s: latest release %s, running %s", t->name, tag, running[0] ? running : "unknown");
            xSemaphoreTake(s_lock, portMAX_DELAY);
            snprintf(t->latest, sizeof(t->latest), "%s", tag);
            snprintf(t->url, sizeof(t->url), "%s", url);
            t->src = s_src->id;
            t->beta = s_beta;
            xSemaphoreGive(s_lock);
            set(t, newer ? "available" : "current", 0, NULL);
        }
    } else if (act == DO_INSTALL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(tag, sizeof(tag), "%s", t->latest);
        snprintf(url, sizeof(url), "%s", t->url);
        xSemaphoreGive(s_lock);
        set(t, "downloading", 0, NULL);
        if (target == ESP) {
            const char *state;
            const char *why = install_esp(url, &state);
            set(t, state, 100, why);
            if (!why) {
                ESP_LOGI(TAG, "open-z1 %s installed; restarting", tag);
                vTaskDelay(pdMS_TO_TICKS(3000));  // let the page see "restarting"
                esp_restart();
            }
            ESP_LOGW(TAG, "update failed: %s", why);
        } else {
            const char *why = download_lpc(t, url);
            if (why) {
                ESP_LOGW(TAG, "motion controller update: %s", why);
                if (strcmp(t->state, "failed") != 0) {
                    set(t, "failed", 0, why);
                }
            } else {
                watch_lpc(t);
            }
        }
    } else {
        watch_lpc(t);
    }
    atomic_store(&s_running, false);
    vTaskDelete(NULL);
}

static bool start_task(int target, action_t act)
{
    if (xTaskCreate(task, "fwupdate", TASK_STACK, (void *)(intptr_t)(act << 1 | target), 4, NULL) == pdPASS) {
        return true;
    }
    atomic_store(&s_running, false);
    set(&s_t[target], "failed", 0, "out of memory");
    return false;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    char line[160];
    snprintf(line, sizeof(line), "%s\n", text);
    return httpd_resp_sendstr(req, line);
}

static esp_err_t update_get(httpd_req_t *req)
{
    if (!ota_host_ok(req)) {
        return reply(req, "403 Forbidden", "refused");
    }
    char out[640];
    int n = snprintf(out, sizeof(out), "{");
    for (int i = 0; i < 2; i++) {
        char running[24];
        running_version(i, running, sizeof(running));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        // msg holds fixed texts (ours, lpcfw's); latest passed json_str.
        n += snprintf(out + n, sizeof(out) - n,
                      "%s\"%s\":{\"state\":\"%s\",\"current\":\"%s\",\"latest\":\"%s\",\"src\":\"%s\","
                      "\"beta\":%s,\"pct\":%d,\"msg\":\"%s\"}",
                      i ? "," : "", s_t[i].name, s_t[i].state, running, s_t[i].latest, s_t[i].src,
                      s_t[i].beta ? "true" : "false", s_t[i].pct, s_t[i].msg);
        xSemaphoreGive(s_lock);
    }
    snprintf(out + n, sizeof(out) - n, "}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (!ota_same_origin(req)) {
        return reply(req, "403 Forbidden", "cross-site request refused");
    }
    char body[40] = "";
    int n = req->content_len < sizeof(body) ? httpd_req_recv(req, body, req->content_len) : -1;
    if (n < 0) {
        return reply(req, "400 Bad Request", "body must be: check|install esp|lpc [beta] [community]");
    }
    body[n] = '\0';
    char verb[8] = "", which[4] = "";
    sscanf(body, "%7s %3s", verb, which);
    bool beta = strstr(body, " beta") != NULL, community = strstr(body, " community") != NULL;
    int target = !strcmp(which, "esp") ? ESP : !strcmp(which, "lpc") ? LPC : -1;
    bool install = !strcmp(verb, "install");
    if (target < 0 || (!install && strcmp(verb, "check") != 0)) {
        return reply(req, "400 Bad Request", "body must be: check|install esp|lpc [beta] [community]");
    }
    target_t *t = &s_t[target];
    if (install && strcmp(t->state, "available") != 0 && strcmp(t->state, "current") != 0) {
        return reply(req, "409 Conflict", "check for an update first");
    }
    // Install exactly what was checked: same source and channel as asked now.
    const source_t *src = target == ESP ? &ESP_SRC : community ? &COMMUNITY_SRC : &MAKERA_SRC;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stale = install && (strcmp(t->src, src->id) != 0 || t->beta != beta);
    xSemaphoreGive(s_lock);
    if (stale) {
        return reply(req, "409 Conflict", "the last check was for another source or channel; check again");
    }
    if (atomic_exchange(&s_running, true)) {
        return reply(req, "409 Conflict", "another update or check is running");
    }
    s_beta = beta;
    s_src = src;
    set(t, install ? "downloading" : "checking", 0, NULL);
    if (!start_task(target, install ? DO_INSTALL : DO_CHECK)) {
        return reply(req, "500 Internal Server Error", "out of memory");
    }
    return reply(req, "200 OK", "started");
}

// What the first bytes of an upload say it is: NULL for a motion controller
// file (Makera's package or an LPC1768 image), else why not.
static const char *lpc_file_kind(const uint8_t *b, size_t n, size_t total)
{
    uint32_t w0 = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    if (b[0] == 0xE9) {
        return "an open-z1 Mainboard file: use Upload file in the Mainboard row";
    }
    if (w0 == LPCFW_MAGIC) {
        return NULL;  // lpcfw checks the whole package
    }
    return total <= LPCFW_LPC_MAX && n >= 8 && !lpcfw_lpc_vectors_ok(b, total)
               ? NULL
               : "not a motion controller firmware file (lpc1768.bin, or Makera's MakeraZ1-...-bin.bin)";
}

// The motion controller's file from the page: received into LPC_TMP, then as
// a download from GitHub.
static esp_err_t lpcfw_post(httpd_req_t *req)
{
    if (!ota_same_origin(req)) {
        return reply(req, "403 Forbidden", "cross-site request refused");
    }
    if (req->content_len < 8 || req->content_len > UPLOAD_MAX) {
        return reply(req, "413 Content Too Large", "no file, or far too large for motion controller firmware");
    }
    if (atomic_exchange(&s_running, true)) {
        return reply(req, "409 Conflict", "another update or check is running");
    }
    target_t *t = &s_t[LPC];
    const char *why = lpc_refused();
    if (why) {
        atomic_store(&s_running, false);
        return reply(req, "409 Conflict", why);
    }
    if (!storage_hold()) {
        atomic_store(&s_running, false);
        return reply(req, "409 Conflict", "no SD card");
    }
    FILE *f = fopen(LPC_TMP, "wb");
    size_t left = req->content_len;
    bool first = true;
    int retries = 0;
    const char *status = "400 Bad Request";
    why = f ? NULL : "cannot write the SD card";
    while (!why && left > 0) {
        int n = httpd_req_recv(req, (char *)s_buf, left < sizeof(s_buf) ? left : sizeof(s_buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++retries <= RECV_RETRIES) {
            continue;
        }
        if (n <= 0) {
            why = "connection lost";
            break;
        }
        retries = 0;
        while (first && n < 8 && (size_t)n < left) {  // the first piece can be shorter
            int m = httpd_req_recv(req, (char *)s_buf + n, 8 - n);
            if (m == HTTPD_SOCK_ERR_TIMEOUT && ++retries <= RECV_RETRIES) {
                continue;
            }
            if (m <= 0) {
                n = -1;
                break;
            }
            n += m;
        }
        if (n < 0) {
            why = "connection lost";
            break;
        }
        if (first && (why = lpc_file_kind(s_buf, n, req->content_len)) != NULL) {
            break;
        }
        first = false;
        if (fwrite(s_buf, 1, n, f) != (size_t)n) {
            why = "cannot write the SD card";
        }
        left -= n;
    }
    if (f && (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0) && !why) {
        why = "cannot write the SD card";
    }
    if (!why) {
        status = "409 Conflict";
        why = lpc_start();  // lpcfw checks it and tells why not
    } else {
        remove(LPC_TMP);
    }
    storage_release();
    if (why) {
        atomic_store(&s_running, false);
        ESP_LOGW(TAG, "motion controller file refused: %s", why);
        return reply(req, status, why);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    t->latest[0] = '\0';
    t->src = "";
    xSemaphoreGive(s_lock);
    set(t, "installing", 0, NULL);  // before the reply: the page's next poll sees it
    start_task(LPC, DO_WATCH);      // lpcfw goes on even if this fails; the log follows it
    return reply(req, "200 OK", "installing; the machine will home");
}

esp_err_t fwupdate_start(void)
{
    httpd_handle_t h = ota_httpd();
    if (!h) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/api/update", .method = HTTP_GET, .handler = update_get},
        {.uri = "/api/update", .method = HTTP_POST, .handler = update_post},
        {.uri = "/api/lpcfw", .method = HTTP_POST, .handler = lpcfw_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(h, &uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
