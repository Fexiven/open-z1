#include "wifi_mgr.h"
#include "wifi_mgr_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "mbedtls/platform_util.h"
#include "sdlog.h"  // SDLOG_CONSOLE_ONLY: lines with an SSID stay off the card

static const char *TAG = "wifi_mgr";

_Static_assert((int)WIFI_POLICY_REASON_4WAY_HANDSHAKE_TIMEOUT == (int)WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, "reason mismatch");
_Static_assert((int)WIFI_POLICY_REASON_NO_AP_FOUND == (int)WIFI_REASON_NO_AP_FOUND, "reason mismatch");
_Static_assert((int)WIFI_POLICY_REASON_AUTH_FAIL == (int)WIFI_REASON_AUTH_FAIL, "reason mismatch");
_Static_assert((int)WIFI_POLICY_REASON_HANDSHAKE_TIMEOUT == (int)WIFI_REASON_HANDSHAKE_TIMEOUT, "reason mismatch");
_Static_assert((int)WIFI_POLICY_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY == (int)WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY,
               "reason mismatch");
_Static_assert((int)WIFI_POLICY_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD == (int)WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD,
               "reason mismatch");
_Static_assert((int)WIFI_POLICY_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD == (int)WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD,
               "reason mismatch");

#define NVS_NS "wifi_config"
#define DISCONNECTED_BIT BIT0
#define GOT_IP_BIT BIT1
#define DISCONNECT_WAIT_MS 3000
#define SCAN_MAX 32
#define STEER_GRACE_MS 10000

// s_lock guards the state below; s_op serialises the blocking operations
// (switching networks, scanning).
static SemaphoreHandle_t s_lock, s_op;
static EventGroupHandle_t s_events;
static esp_timer_handle_t s_retry_timer;
static bool s_started;

static wifi_mgr_status_t s_status;
static bool s_assoc;              // driver says associated (STA_CONNECTED .. STA_DISCONNECTED)
static bool s_connecting;         // esp_wifi_connect() issued, no outcome yet
static bool s_expect_disconnect;  // we asked for the next disconnect; don't retry
static bool s_paused;             // an operation is reconfiguring; the retry timer must not connect
static bool s_ever_connected;     // the current credentials have worked at least once

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static esp_err_t load_credentials(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(h, "ssid", ssid, &ssid_cap);
    if (err == ESP_OK) {
        err = nvs_get_str(h, "password", pass, &pass_cap);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            pass[0] = '\0';
            err = ESP_OK;
        }
    }
    nvs_close(h);
    return err;
}

static esp_err_t save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "password", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t apply_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(cfg.sta.password, pass, strlen(pass));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    // With several APs for one SSID (mesh, repeaters) the default fast scan
    // joins whichever answers first, often the weakest.
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    // 802.11k/v: lets a mesh (e.g. FRITZ!Box Mesh steering) move us to a better AP.
    cfg.sta.rm_enabled = 1;
    cfg.sta.btm_enabled = 1;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    mbedtls_platform_zeroize(&cfg, sizeof(cfg));
    return err;
}

// The only place that schedules a connect attempt.
static void request_connect(uint32_t delay_ms)
{
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

// Drops a BSSID/channel pin from the STA config. With btm_enabled the driver
// pins the chosen AP while connecting and fails to clear it on connect ("sta
// is connecting, cannot set config"). A pinned config blocks 802.11v
// steering and would make our retries chase one AP that may be gone.
// Returns whether a pin was removed.
static bool clear_ap_pin(void)
{
    wifi_config_t cfg;
    bool pinned = esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && (cfg.sta.bssid_set || cfg.sta.channel);
    if (pinned) {
        cfg.sta.bssid_set = false;
        cfg.sta.channel = 0;
        pinned = esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK;
    }
    mbedtls_platform_zeroize(&cfg, sizeof(cfg));
    return pinned;
}

static void retry_cb(void *arg)
{
    lock();
    bool go = s_status.configured && !s_paused && !s_assoc && !s_connecting;
    if (go) {
        s_connecting = true;
    }
    unlock();
    if (!go) {
        return;
    }
    clear_ap_pin();
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        lock();
        s_connecting = false;
        unsigned attempt = s_status.failed_attempts++;
        unlock();
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
        request_connect(wifi_policy_backoff_ms(attempt));
    }
}

static void on_disconnected(const wifi_event_sta_disconnected_t *ev)
{
    lock();
    bool was_connected = s_status.connected;
    s_assoc = false;
    s_connecting = false;
    s_status.connected = false;
    s_status.ip[0] = '\0';
    if (s_expect_disconnect) {
        s_expect_disconnect = false;
        unlock();
        xEventGroupSetBits(s_events, DISCONNECTED_BIT);
        return;
    }
    if (!s_status.configured || s_paused) {
        unlock();
        return;
    }
    if (ev->reason == WIFI_REASON_BSS_TRANSITION_DISASSOC || ev->reason == WIFI_REASON_ROAMING) {
        // An 802.11v transition: the supplicant is already connecting to the
        // AP the router steered us to, and a connect of ours would collide
        // with it. Only step in if that has not worked out in time.
        unlock();
        ESP_LOGI(TAG, "steered to another AP");
        request_connect(STEER_GRACE_MS);
        return;
    }
    s_status.last_reason = ev->reason;
    s_status.last_fail = wifi_policy_classify(ev->reason, s_ever_connected);
    unsigned attempt = s_status.failed_attempts++;
    unlock();

    uint32_t delay = wifi_policy_backoff_ms(attempt);
    ESP_LOGW(TAG, "%s (reason %d), retrying in %lu ms", was_connected ? "connection lost" : "connect failed",
             ev->reason, (unsigned long)delay);
    request_connect(delay);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        request_connect(1000);
        break;
    case WIFI_EVENT_STA_CONNECTED:
        lock();
        s_assoc = true;
        s_connecting = false;
        unlock();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        on_disconnected(data);
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    lock();
    s_status.connected = true;
    s_ever_connected = true;
    s_status.failed_attempts = 0;
    s_status.last_fail = WIFI_FAIL_NONE;
    s_status.last_reason = 0;
    snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ev->ip_info.ip));
    xEventGroupSetBits(s_events, GOT_IP_BIT);
    char ssid[sizeof(s_status.ssid)], ip[sizeof(s_status.ip)];
    strcpy(ssid, s_status.ssid);
    strcpy(ip, s_status.ip);
    unlock();
    SDLOG_CONSOLE_ONLY(ESP_LOGI(TAG, "connected to \"%s\", ip %s", ssid, ip));
    if (clear_ap_pin()) {
        ESP_LOGI(TAG, "cleared the AP pin left from connecting, so the network can steer us");
    }
}

// Pauses reconnects and, if the driver is associated or mid-attempt, tears
// that down and waits for the resulting disconnect. Caller holds s_op.
static void quiesce(bool keep_association)
{
    lock();
    s_paused = true;
    bool tear_down = keep_association ? (s_connecting && !s_assoc) : (s_connecting || s_assoc);
    if (tear_down) {
        s_expect_disconnect = true;
        xEventGroupClearBits(s_events, DISCONNECTED_BIT);
    }
    unlock();
    esp_timer_stop(s_retry_timer);
    if (!tear_down) {
        return;
    }
    if (esp_wifi_disconnect() != ESP_OK ||
        !(xEventGroupWaitBits(s_events, DISCONNECTED_BIT, pdTRUE, pdTRUE, pdMS_TO_TICKS(DISCONNECT_WAIT_MS)) &
          DISCONNECTED_BIT)) {
        ESP_LOGW(TAG, "no disconnect confirmation; continuing");
    }
    lock();
    s_expect_disconnect = false;
    s_assoc = false;
    s_connecting = false;
    unlock();
}

static void resume(void)
{
    lock();
    s_paused = false;
    bool want = s_status.configured && !s_assoc;
    unlock();
    if (want) {
        request_connect(1000);
    }
}

esp_err_t wifi_mgr_start(void)
{
    if (s_lock) {
        return ESP_ERR_INVALID_STATE;  // one attempt only; a failed start is not retried
    }
    s_lock = xSemaphoreCreateMutex();
    s_op = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_lock || !s_op || !s_events) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;  // already created by someone else
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    uint8_t mac[6];
    char hostname[24];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hostname, sizeof(hostname), "makera-z1-%02x%02x", mac[4], mac[5]);
    esp_netif_set_hostname(netif, hostname);

    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_timer_create(&targs, &s_retry_timer)) != ESP_OK ||
        (err = esp_wifi_init(&icfg)) != ESP_OK ||
        // Credentials live only in our NVS keys, not in the driver's own copy.
        (err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL)) != ESP_OK ||
        (err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL)) != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return err;
    }

    char ssid[WIFI_SSID_MAX + 1], pass[WIFI_PASSWORD_MAX + 1];
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK && wifi_policy_creds_valid(ssid, pass) &&
        apply_config(ssid, pass) == ESP_OK) {
        s_status.configured = true;
        strcpy(s_status.ssid, ssid);
        SDLOG_CONSOLE_ONLY(ESP_LOGI(TAG, "hostname %s, joining \"%s\"", hostname, ssid));
    } else {
        ESP_LOGW(TAG, "hostname %s, no Wi-Fi configured (use: wifi set \"<ssid>\" \"<password>\")", hostname);
    }
    mbedtls_platform_zeroize(pass, sizeof(pass));

    wifi_setup_ap_init();  // before the start: puts the hotspot's config into the driver
    err = esp_wifi_start();
    s_started = err == ESP_OK;
    return err;
}

void wifi_mgr_op_begin(void)
{
    xSemaphoreTake(s_op, portMAX_DELAY);
    quiesce(true);
}

void wifi_mgr_op_end(void)
{
    resume();
    xSemaphoreGive(s_op);
}

// Puts ssid/password into the driver and the status, for the next connect.
static esp_err_t switch_to(const char *ssid, const char *password)
{
    esp_err_t err = apply_config(ssid, password);
    if (err == ESP_OK) {
        lock();
        s_status.configured = true;
        s_status.failed_attempts = 0;
        s_status.last_fail = WIFI_FAIL_NONE;
        s_status.last_reason = 0;
        s_ever_connected = false;
        s_status.off = false;
        strcpy(s_status.ssid, ssid);
        unlock();
    }
    return err;
}

esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *password)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_policy_creds_valid(ssid, password)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_op, portMAX_DELAY);
    quiesce(false);

    esp_err_t err = switch_to(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rejected new network: %s", esp_err_to_name(err));
        resume();  // the old configuration is still in the driver
        xSemaphoreGive(s_op);
        return err;
    }

    // Saved only after the radio accepted it, so NVS never holds a config
    // the driver refused. If saving fails we still join, just not persistently.
    err = save_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not save Wi-Fi to NVS (%s); joining for this boot only", esp_err_to_name(err));
    }
    SDLOG_CONSOLE_ONLY(ESP_LOGI(TAG, "switching to \"%s\"", ssid));
    resume();
    xSemaphoreGive(s_op);
    return ESP_OK;
}

// The app's join: switches to the given network and keeps it only if it works:
// waits up to timeout_ms for an IP (retrying as usual meanwhile), then saves it
// to NVS and copies the address into ip. Otherwise goes back to the saved
// network and returns ESP_FAIL. Blocks for the whole attempt.
// ESP_ERR_INVALID_ARG: the credentials are not valid.
static esp_err_t try_credentials(const char *ssid, const char *password, uint32_t timeout_ms, char ip[16])
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_policy_creds_valid(ssid, password)) {
        return ESP_ERR_INVALID_ARG;
    }
    char old_ssid[WIFI_SSID_MAX + 1], old_pass[WIFI_PASSWORD_MAX + 1];
    bool had_old = load_credentials(old_ssid, sizeof(old_ssid), old_pass, sizeof(old_pass)) == ESP_OK &&
                   wifi_policy_creds_valid(old_ssid, old_pass);
    xSemaphoreTake(s_op, portMAX_DELAY);
    quiesce(false);
    xEventGroupClearBits(s_events, GOT_IP_BIT);
    esp_err_t err = switch_to(ssid, password);
    if (err == ESP_OK) {
        SDLOG_CONSOLE_ONLY(
            ESP_LOGI(TAG, "trying \"%s\" for up to %lu s", ssid, (unsigned long)(timeout_ms / 1000)));
        resume();
        bool joined = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & GOT_IP_BIT;
        if (joined) {
            lock();
            snprintf(ip, 16, "%s", s_status.ip);
            unlock();
            esp_err_t save = save_credentials(ssid, password);
            if (save != ESP_OK) {
                ESP_LOGE(TAG, "could not save Wi-Fi to NVS (%s); joined for this boot only", esp_err_to_name(save));
            }
        } else {
            // Back to the network that worked, so the machine stays reachable.
            SDLOG_CONSOLE_ONLY(ESP_LOGW(TAG, "\"%s\" did not connect; back to the saved network", ssid));
            quiesce(false);
            if (!had_old || switch_to(old_ssid, old_pass) != ESP_OK) {
                lock();
                s_status.configured = false;
                unlock();
            }
            err = ESP_FAIL;
        }
    }
    mbedtls_platform_zeroize(old_pass, sizeof(old_pass));
    resume();
    xSemaphoreGive(s_op);
    return err;
}

// Leaves the network and stays off it until a new network is set or the next
// boot; the saved credentials stay.
static esp_err_t disconnect_station(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_op, portMAX_DELAY);
    quiesce(false);
    lock();
    s_status.configured = false;  // no reconnects until a new network or the next boot
    s_status.off = true;
    unlock();
    resume();
    xSemaphoreGive(s_op);
    ESP_LOGI(TAG, "disconnected on request; the saved network is used again at the next boot");
    return ESP_OK;
}

esp_err_t wifi_mgr_scan(wifi_mgr_ap_t *out, size_t cap, size_t *count)
{
    *count = 0;
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_ap_record_t *recs = calloc(SCAN_MAX, sizeof(*recs));
    if (!recs) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_op, portMAX_DELAY);
    quiesce(true);

    const wifi_scan_config_t cfg = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    uint16_t n = SCAN_MAX;
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&n, recs);
    }
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < n && *count < cap; i++) {
            wifi_mgr_ap_t *ap = &out[(*count)++];
            snprintf(ap->ssid, sizeof(ap->ssid), "%s", (const char *)recs[i].ssid);
            memcpy(ap->bssid, recs[i].bssid, sizeof(ap->bssid));
            ap->channel = recs[i].primary;
            ap->rssi = recs[i].rssi;
            ap->open = recs[i].authmode == WIFI_AUTH_OPEN;
        }
    }
    free(recs);
    resume();
    xSemaphoreGive(s_op);
    return err;
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    *out = s_status;
    unlock();
    wifi_ap_record_t ap;
    if (out->connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
    }
}

// ---- the app's `wlan` commands

#define WLAN_JOIN_MS 20000  // INFERRED settle window: some APs refuse the first try

static struct {
    wifi_app_cmd_t cmd;
    char ssid[WIFI_SSID_MAX + 1];
    char pass[WIFI_PASSWORD_MAX + 1];
    wifi_mgr_reply_fn reply;
    void *ctx;
} s_app;  // filled while !s_app_busy, then the app task's
static volatile bool s_app_busy;
static TaskHandle_t s_app_task;

static void app_scan(void)
{
    static wifi_mgr_ap_t aps[24];
    static char text[24 * 48];
    size_t n = 0, len = 0;
    esp_err_t err = wifi_mgr_scan(aps, sizeof(aps) / sizeof(aps[0]), &n);
    if (err != ESP_OK) {
        snprintf(text, sizeof(text), "WiFi scan failed: %.100s\n", esp_err_to_name(err));  // stock's text
        s_app.reply(s_app.ctx, text, false);
        return;
    }
    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);
    text[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        bool dup = false;  // results come strongest first; stock keeps the strongest of each SSID
        for (size_t k = 0; k < i && !dup; k++) {
            dup = !strcmp(aps[k].ssid, aps[i].ssid);
        }
        int w = dup ? -1
                    : wifi_policy_scan_line(text + len, sizeof(text) - len, aps[i].ssid, aps[i].open, aps[i].rssi,
                                            st.connected && !strcmp(st.ssid, aps[i].ssid));
        if (w > 0) {
            len += (size_t)w;
        }
    }
    ESP_LOGI(TAG, "wlan scan: %u networks", (unsigned)n);
    s_app.reply(s_app.ctx, text, true);
}

static void app_join(void)
{
    char ip[16] = "", text[64];
    esp_err_t err = try_credentials(s_app.ssid, s_app.pass, WLAN_JOIN_MS, ip);
    mbedtls_platform_zeroize(s_app.pass, sizeof(s_app.pass));
    if (err == ESP_OK) {
        snprintf(text, sizeof(text), "WiFi connected, ip: %s\n", ip);  // stock's texts
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(text, sizeof(text), "Error: Failed to set WiFi config: %s\n", esp_err_to_name(err));
    } else {
        snprintf(text, sizeof(text), "Error: Failed to connect to AP\n");
    }
    SDLOG_CONSOLE_ONLY(ESP_LOGI(TAG, "wlan join \"%s\": %s", s_app.ssid,
                                err == ESP_OK ? ip : "failed, back on the saved network"));
    // With a new network the asking client is usually gone by now (it was on the old one).
    s_app.reply(s_app.ctx, text, err == ESP_OK);
}

static void app_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_app.cmd == WIFI_APP_SCAN) {
            app_scan();
        } else if (s_app.cmd == WIFI_APP_JOIN) {
            app_join();
        } else {
            // Replied first: once off the network the app can no longer hear us.
            s_app.reply(s_app.ctx, "WiFi Disconnected!\n", true);  // stock's text
            vTaskDelay(pdMS_TO_TICKS(500));
            disconnect_station();
        }
        mbedtls_platform_zeroize(s_app.pass, sizeof(s_app.pass));
        s_app_busy = false;
    }
}

bool wifi_mgr_app_command(wifi_app_cmd_t cmd, const char *ssid, const char *password, wifi_mgr_reply_fn reply,
                          void *ctx)
{
    if (s_app_busy || (!s_app_task && xTaskCreate(app_task, "wifi_app", 6144, NULL, 3, &s_app_task) != pdPASS)) {
        return false;
    }
    s_app.cmd = cmd;
    snprintf(s_app.ssid, sizeof(s_app.ssid), "%s", ssid ? ssid : "");
    snprintf(s_app.pass, sizeof(s_app.pass), "%s", password ? password : "");
    s_app.reply = reply;
    s_app.ctx = ctx;
    s_app_busy = true;
    xTaskNotifyGive(s_app_task);
    return true;
}

bool wifi_mgr_app_busy(void)
{
    return s_app_busy;
}
