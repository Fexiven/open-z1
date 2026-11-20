#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi_policy.h"

typedef struct {
    bool configured;
    bool off;  // left the network on request (wlan -d) until a new one or the next boot
    bool connected;
    char ssid[WIFI_SSID_MAX + 1];
    char ip[16];
    int8_t rssi;
    wifi_fail_t last_fail;
    int last_reason;
    unsigned failed_attempts;
} wifi_mgr_status_t;

typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    bool open;
} wifi_mgr_ap_t;

// Starts Wi-Fi in station mode with the credentials stored in NVS (namespace
// "wifi_config", keys "ssid"/"password", as on the stock firmware) and keeps
// reconnecting with backoff; the setup hotspot below may open. Call once, before
// anything else in this API.
esp_err_t wifi_mgr_start(void);

// Two ways to set a network, saved at different times on purpose:
//  - wifi_mgr_set_credentials (the USB console's `wifi set`) saves at once, so
//    it always works as the recovery path, even for a network out of reach;
//  - the app's `wlan` (wifi_mgr_app_command) saves only after an IP, and
//    otherwise goes back, so the app can never strand the machine.
//
// Switches to the given network and saves it to NVS once the radio accepted it.
// Returns ESP_OK once the switch has started, even if saving failed (logged).
// Blocks for up to a few seconds while an ongoing connection is torn down.
esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *password);

// Scans for networks. Blocks for the scan (about 2-4 s). An established
// connection is kept; a connect attempt in progress is restarted afterwards.
esp_err_t wifi_mgr_scan(wifi_mgr_ap_t *out, size_t cap, size_t *count);

void wifi_mgr_get_status(wifi_mgr_status_t *out);

// The app's `wlan` commands, run in wifi_mgr's own task (a scan takes seconds,
// a join up to 20 s). reply(ctx, text, ok) is called once, from that task,
// with stock's wording; ssid and password are copied (the password is wiped
// after use). Returns false if one is still running.
typedef enum { WIFI_APP_SCAN, WIFI_APP_JOIN, WIFI_APP_OFF } wifi_app_cmd_t;
typedef void (*wifi_mgr_reply_fn)(void *ctx, const char *text, bool ok);
bool wifi_mgr_app_command(wifi_app_cmd_t cmd, const char *ssid, const char *password, wifi_mgr_reply_fn reply,
                          void *ctx);
bool wifi_mgr_app_busy(void);

// ---- first-time setup hotspot
//
// A WPA2 access point "Makera_Z1_XXXX" (stock's name: the last two STA MAC
// bytes) whose passphrase is the machine serial (NVS factory/machine_sn, on
// the device label), or, without a usable one, a random 12-character password
// printed once on the USB console (never logged). It opens only within
// CONFIG_Z1_WIFI_SETUP_WINDOW_S (10 min) after boot, before the station's
// first IP, and when no network is configured or the saved one has given no
// IP for CONFIG_Z1_WIFI_SETUP_UNREACHABLE_S (3 min). APSTA: the station keeps
// retrying meanwhile. Once the station has an IP, the hotspot closes 30 s
// later (so the setup page can show the address) and stays closed until the
// next boot. Its clients get only the setup page (components/webui): every
// other server refuses them (wifi_mgr_on_setup_ap).

bool wifi_mgr_setup_ap_active(void);

// Whether the connected socket fd came in over the setup hotspot (to its
// address, or from its subnet); false while the hotspot is off.
bool wifi_mgr_on_setup_ap(int fd);

// USB console (`wifi setup-ap`): opens a hotspot window now, for
// CONFIG_Z1_WIFI_SETUP_WINDOW_S, whatever the rules say; the station's next
// IP still closes it.
void wifi_mgr_setup_ap_force(void);

// One line for the console's `wifi status`.
void wifi_mgr_setup_ap_describe(char *out, size_t cap);
