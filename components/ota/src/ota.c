#include "ota.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_origin.h"
#include "ota_httpd.h"
#include "ota_install.h"
#include "ota_version.h"
#include "wifi_mgr.h"

// Without this a POST /ota would install any image from the LAN.
#if !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
#error "OTA needs signed app verification (CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT)"
#endif

static const char *TAG = "ota";

#define OTA_PORT 80

#define BUNDLE_MAGIC 0x4D5173EEu  // stock MakeraZ1-<ver>-bin.bin (split_z1_bundle.py)
#define RECV_RETRIES 7            // each waits recv_wait_timeout (2 s)
// The rollback restart waits for a running job at most this long, so a job
// state that never ends cannot keep an unhealthy image forever.
#define HOLD_MAX_S (12 * 3600)

#if CONFIG_Z1_OTA_TEST_NEVER_VALID
#define NEVER_VALID true
#else
#define NEVER_VALID false
#endif

static ota_busy_fn s_busy;
static bool (*s_healthy)(void), (*s_hold)(void);  // for confirm_task
static atomic_bool s_active;   // set before s_busy() is asked: see ota_active()
static atomic_bool s_pending;  // running image not yet confirmed
static uint8_t s_buf[4096];    // only the httpd task uses it
static httpd_handle_t s_httpd;

bool ota_active(void)
{
    return atomic_load(&s_active);
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    char line[224];
    snprintf(line, sizeof(line), "%s\n", text);
    return httpd_resp_sendstr(req, line);
}

static esp_err_t info_get(httpd_req_t *req)
{
    if (!ota_host_ok(req)) {
        return reply(req, "403 Forbidden", "refused");
    }
    char sha[CONFIG_APP_RETRIEVE_LEN_ELF_SHA + 1];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    const esp_app_desc_t *app = esp_app_get_description();
    char lpc[64];
    cfg_lpc_status(lpc, sizeof(lpc));
    char line[200];
    snprintf(line, sizeof(line), "%s %s %s %s %s\nlpc_config=%s", app->project_name, app->version, sha,
             esp_ota_get_running_partition()->label, atomic_load(&s_pending) ? "pending" : "valid", lpc);
    return reply(req, "200 OK", line);
}

const char *ota_check_desc(const esp_app_desc_t *new, bool force, const char **status)
{
    const esp_app_desc_t *app = esp_app_get_description();
    *status = "400 Bad Request";
    if (strncmp(new->project_name, app->project_name, sizeof(new->project_name)) != 0) {
        ESP_LOGW(TAG, "image is %.32s, this board runs %s", new->project_name, app->project_name);
        return "built for another board or flash layout";
    }
    if (!force && ota_version_cmp(new->version, app->version) < 0) {
        ESP_LOGW(TAG, "image %.32s is older than %s", new->version, app->version);
        return "older than the running version";
    }
    return NULL;
}

// Receives the body into the other slot. Returns NULL on success, else why not
// (and sets *status).
static const char *receive(httpd_req_t *req, bool force, const char **status)
{
    *status = "500 Internal Server Error";
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return "no OTA slot";
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        *status = "413 Content Too Large";
        return "no image, or larger than the slot";
    }
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin: %s", esp_err_to_name(err));
        return "cannot start writing the slot";
    }
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)req->content_len, part->label);
    size_t left = req->content_len;
    bool first = true;
    int retries = 0;
    const char *why = NULL;
    while (left > 0) {
        int n = httpd_req_recv(req, (char *)s_buf, left < sizeof(s_buf) ? left : sizeof(s_buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++retries <= RECV_RETRIES) {
            continue;
        }
        if (n <= 0) {
            why = "connection lost";
            break;
        }
        retries = 0;
        // esp_ota_write would refuse it too (first byte not 0xE9); this says why.
        if (first && n >= 8 && (s_buf[0] | s_buf[1] << 8 | s_buf[2] << 16 | (uint32_t)s_buf[3] << 24) == BUNDLE_MAGIC) {
            *status = "400 Bad Request";
            why = "a Makera firmware package: use Upload file in the Motion controller row";
            break;
        }
        // An LPC1768 image starts with its stack pointer (in its RAM) and a
        // Thumb reset address (odd); an ESP32-S3 image with 0xE9.
        uint32_t sp = s_buf[0] | s_buf[1] << 8 | s_buf[2] << 16 | (uint32_t)s_buf[3] << 24;
        if (first && n >= 8 && sp >> 16 == 0x1000 && (s_buf[4] & 1)) {
            *status = "400 Bad Request";
            why = "a motion controller firmware file: use Upload file in the Motion controller row";
            break;
        }
        first = false;
        err = esp_ota_write(h, s_buf, n);
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            *status = "400 Bad Request";
            why = "not an ESP32-S3 app image";
            break;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write: %s", esp_err_to_name(err));
            why = "flash write failed";
            break;
        }
        left -= n;
    }
    if (why) {
        esp_ota_abort(h);
        return why;
    }
    err = esp_ota_end(h);  // checks the image and its signature
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        *status = "400 Bad Request";
        return "image damaged or not signed with this firmware's key";
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "end: %s", esp_err_to_name(err));
        return "image check failed";
    }
    esp_app_desc_t new_app;
    if (esp_ota_get_partition_description(part, &new_app) != ESP_OK) {
        return "image has no app description";
    }
    const char *why_not = ota_check_desc(&new_app, force, status);
    if (why_not) {
        return why_not;
    }
    const char *busy = ota_busy_now();
    if (busy) {
        *status = "409 Conflict";
        return busy;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot: %s", esp_err_to_name(err));
        return "cannot select the new image";
    }
    return NULL;
}

// flash.py install clears the old table at 0x8000 in its last step; until
// then a rerun of it, not an update, must come next.
static bool migration_unfinished(void)
{
    uint8_t magic[2];
    return esp_flash_read(NULL, magic, 0x8000, sizeof(magic)) == ESP_OK && magic[0] == 0xAA && magic[1] == 0x50;
}

httpd_handle_t ota_httpd(void)
{
    return s_httpd;
}

bool ota_host_ok(httpd_req_t *req)
{
    char host[64];
    const char *own = "";
    esp_netif_get_hostname(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &own);
    if (wifi_mgr_on_setup_ap(httpd_req_to_sockfd(req))) {
        return false;  // the setup hotspot gets only its setup page (components/webui)
    }
    // A Host that does not fit, or none, counts as foreign.
    return httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK && http_host_ok(host, own ? own : "");
}

bool ota_same_origin(httpd_req_t *req)
{
    if (!ota_host_ok(req)) {
        return false;
    }
    char origin[80], host[64];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    if (err == ESP_ERR_NOT_FOUND) {
        return true;
    }
    // An Origin that does not fit counts as foreign.
    return err == ESP_OK && httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK &&
           http_origin_ok(origin, host);
}

const char *ota_busy_now(void)
{
    // A job armed just before the update began (or a button restart) must not
    // be cut off by the restart. From here until the restart play refuses new
    // jobs (ota_active()).
    const char *busy = s_busy ? s_busy() : NULL;
    if (busy) {
        ESP_LOGW(TAG, "not installed: %s", busy);
    }
    return busy;
}

const char *ota_claim(void)
{
    if (migration_unfinished()) {
        return "flash layout migration not finished; rerun flash.py install";
    }
    if (atomic_load(&s_pending)) {
        return "the running image is not confirmed yet";
    }
    if (atomic_exchange(&s_active, true)) {
        return "an update is already running";
    }
    // s_active is set first, so a job or upload that starts from now on is
    // refused, and one that started before shows up here.
    const char *why = s_busy ? s_busy() : NULL;
    if (why) {
        atomic_store(&s_active, false);
        ESP_LOGW(TAG, "update refused: %s", why);
    }
    return why;
}

void ota_release(void)
{
    atomic_store(&s_active, false);
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (!ota_same_origin(req)) {
        return reply(req, "403 Forbidden", "cross-site request refused");
    }
    const char *why = ota_claim();
    if (why) {
        return reply(req, "409 Conflict", why);
    }
    char query[32], force[4];
    bool forced = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                  httpd_query_key_value(query, "force", force, sizeof(force)) == ESP_OK && !strcmp(force, "1");
    const char *status;
    why = receive(req, forced, &status);
    if (why) {
        atomic_store(&s_active, false);
        ESP_LOGW(TAG, "update failed: %s", why);
        return reply(req, status, why);
    }
    ESP_LOGI(TAG, "update installed; restarting");
    reply(req, "200 OK", "installed; restarting");
    vTaskDelay(pdMS_TO_TICKS(500));  // let the reply leave
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_start(ota_busy_fn busy)
{
    s_busy = busy;
    esp_ota_img_states_t state;
    atomic_store(&s_pending, esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
                                 state == ESP_OTA_IMG_PENDING_VERIFY);
    httpd_handle_t h;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = OTA_PORT;
    cfg.ctrl_port = 32080;  // must differ from the camera's httpd (32082)
    // The page's polling, a Stop and an upload at once (sdkconfig.defaults:
    // the socket budget). One task serves them all, so a request is short
    // (2 s timeouts); an upload holds it until it ends: Stop over the web
    // waits, the app's Stop on 2222 does not.
    cfg.max_open_sockets = 4;
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 2;
    cfg.stack_size = 8192;  // esp_ota_end verifies the RSA-3072 signature in this task
    cfg.lru_purge_enable = true;  // a dead client never blocks the next update
    cfg.max_uri_handlers = 16;    // /ota (2), the web UI's (11), fwupdate's (3)
    esp_err_t err = httpd_start(&h, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t get = {.uri = "/ota", .method = HTTP_GET, .handler = info_get};
    const httpd_uri_t post = {.uri = "/ota", .method = HTTP_POST, .handler = update_post};
    httpd_register_uri_handler(h, &get);
    httpd_register_uri_handler(h, &post);
    s_httpd = h;
    ESP_LOGI(TAG, "updates on http://<ip>:%d/ota", OTA_PORT);
    return ESP_OK;
}

static void confirm_task(void *arg)
{
    int held_s = 0;
    for (int s = 0; s < OTA_CONFIRM_TIMEOUT_S; s++) {
        if (s_hold() && held_s < HOLD_MAX_S) {
            held_s++;
            s--;  // never restart under a job: the timeout waits, but not forever
        }
        if (!NEVER_VALID && s_healthy()) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                atomic_store(&s_pending, false);
                ESP_LOGI(TAG, "new image confirmed after %d s", s);
                vTaskDelete(NULL);
            }
            ESP_LOGE(TAG, "marking the image valid failed: %s; retrying", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "new image not healthy after %d s; restarting to roll back", OTA_CONFIRM_TIMEOUT_S);
    esp_restart();
}

void ota_confirm(bool (*healthy)(void), bool (*hold)(void))
{
    if (!atomic_load(&s_pending)) {
        return;
    }
    s_healthy = healthy;
    s_hold = hold;
    ESP_LOGI(TAG, "first boot of %s after an update; waiting for Wi-Fi and the control server",
             esp_ota_get_running_partition()->label);
    if (xTaskCreate(confirm_task, "ota_confirm", 3072, NULL, 2, NULL) != pdPASS) {
        esp_restart();  // cannot check it: roll back
    }
}
