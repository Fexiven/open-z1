#include "wifi_policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

wifi_fail_t wifi_policy_classify(int reason, bool ever_connected)
{
    switch (reason) {
    // A wrong WPA2 passphrase shows up as a failed 4-way handshake; WPA3/SAE
    // reports AUTH_FAIL instead.
    case WIFI_POLICY_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_POLICY_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_POLICY_REASON_AUTH_FAIL:
        return ever_connected ? WIFI_FAIL_OTHER : WIFI_FAIL_PASSWORD;
    case WIFI_POLICY_REASON_NO_AP_FOUND:
    case WIFI_POLICY_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_POLICY_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_POLICY_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return WIFI_FAIL_NOT_FOUND;
    default:
        // Includes MIC_FAILURE (14): a TKIP integrity fault on the link, not a credential problem.
        return WIFI_FAIL_OTHER;
    }
}

bool wifi_policy_creds_valid(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return false;
    }
    size_t s = strlen(ssid), p = strlen(password);
    if (s == 0 || s > WIFI_SSID_MAX) {
        return false;
    }
    if (p == 0) {
        return true;
    }
    if (p == WIFI_PASSWORD_MAX) {
        for (size_t i = 0; i < p; i++) {
            if (!isxdigit((unsigned char)password[i])) {
                return false;
            }
        }
        return true;
    }
    if (p < 8 || p > 63) {
        return false;
    }
    for (size_t i = 0; i < p; i++) {
        if (password[i] < 0x20 || password[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

uint32_t wifi_policy_backoff_ms(unsigned attempt)
{
    if (attempt >= 6) {
        return 60000;
    }
    return 1000u << attempt;
}

int wifi_policy_scan_line(char *out, size_t cap, const char *ssid, bool open, int rssi, bool connected)
{
    char esc[WIFI_SSID_MAX + 1];
    size_t n = strlen(ssid);
    if (n == 0 || n > WIFI_SSID_MAX || strchr(ssid, ',') || strchr(ssid, '\n')) {
        return -1;
    }
    for (size_t i = 0; i <= n; i++) {
        esc[i] = ssid[i] == ' ' ? 0x01 : ssid[i];
    }
    int len = snprintf(out, cap, "%s,%d,%d,%d\n", esc, open ? 0 : 1, rssi, connected ? 1 : 0);
    return len < 0 || (size_t)len >= cap ? -1 : len;
}

bool wifi_policy_setup_ap_due(uint32_t uptime_s, bool configured, bool got_ip, uint32_t window_s,
                              uint32_t unreachable_s)
{
    return !got_ip && uptime_s < window_s && (!configured || uptime_s >= unreachable_s);
}

bool wifi_policy_ap_password_ok(const char *pw)
{
    size_t n = pw ? strlen(pw) : 0;
    return n >= 8 && n <= 63 && wifi_policy_creds_valid("x", pw);
}
