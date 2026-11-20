#pragma once
// Pure Wi-Fi decisions, kept free of ESP-IDF so they can be host-tested.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_SSID_MAX 32
#define WIFI_PASSWORD_MAX 64

// The three failure classes the official app distinguishes in its `wlan`
// replies ("Wifi password incorrect!", "No wifi ssid found: <ssid>!",
// "Other error reason!").
typedef enum {
    WIFI_FAIL_NONE = 0,
    WIFI_FAIL_PASSWORD,
    WIFI_FAIL_NOT_FOUND,
    WIFI_FAIL_OTHER,
} wifi_fail_t;

// The ESP-IDF v6.1 wifi_err_reason_t values the classification depends on.
// wifi_mgr.c static-asserts them against the real header.
enum {
    WIFI_POLICY_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
    WIFI_POLICY_REASON_NO_AP_FOUND = 201,
    WIFI_POLICY_REASON_AUTH_FAIL = 202,
    WIFI_POLICY_REASON_HANDSHAKE_TIMEOUT = 204,
    WIFI_POLICY_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY = 210,
    WIFI_POLICY_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD = 211,
    WIFI_POLICY_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD = 212,
};

// reason is an ESP-IDF wifi_err_reason_t value from a STA disconnect event.
// ever_connected: whether these credentials have worked before. Handshake
// failures on a network we already joined are link problems, not a wrong
// password.
wifi_fail_t wifi_policy_classify(int reason, bool ever_connected);

// SSID: 1..32 bytes. Password: empty (open network), 8..63 printable ASCII
// (WPA passphrase), or exactly 64 hex digits (raw PSK).
bool wifi_policy_creds_valid(const char *ssid, const char *password);

// One scan result as the app reads it: "<ssid>,<auth>,<rssi>,<connected>\n",
// spaces in the SSID as 0x01, auth 0 only for an open network (stock's format
// "%s,%d,%d,%d"; INFERRED field order, from the Carvera firmware). Returns the
// length, or -1 for an SSID that cannot be listed (empty, or with a comma).
int wifi_policy_scan_line(char *out, size_t cap, const char *ssid, bool open, int rssi, bool connected);

// Delay before reconnect attempt `attempt` (0-based): 1 s doubling to 60 s.
uint32_t wifi_policy_backoff_ms(unsigned attempt);

// The setup hotspot (wifi_setup_ap.c) is due: within window_s after boot,
// before the station's first IP of this boot, and either no network is
// configured or the saved one has not given an IP for unreachable_s.
bool wifi_policy_setup_ap_due(uint32_t uptime_s, bool configured, bool got_ip, uint32_t window_s,
                              uint32_t unreachable_s);

// A machine serial usable as the hotspot's WPA2 passphrase: 8..63 printable ASCII.
bool wifi_policy_ap_password_ok(const char *pw);
