// The first-time setup hotspot (wifi_mgr.h). A watcher task decides once a
// second; the radio is reconfigured under wifi_mgr's operation lock.

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "wifi_mgr.h"
#include "wifi_mgr_priv.h"

static const char *TAG = "setup_ap";

#define WINDOW_S CONFIG_Z1_WIFI_SETUP_WINDOW_S
#define UNREACHABLE_S CONFIG_Z1_WIFI_SETUP_UNREACHABLE_S
#define LINGER_S 30  // after the first IP, so the setup page can still show it
#define MAX_CLIENTS 2

static esp_netif_t *s_ap_netif;  // created at init, before esp_wifi_start
static atomic_bool s_up;
static atomic_bool s_got_ip;     // the station's first IP since boot (or since a forced window)
static atomic_uint s_ip_at_s;
static atomic_uint s_forced_until_s;  // 0: not forced
static char s_ssid[16];
static bool s_random_pw;  // no usable serial: a random password, printed once

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (!atomic_exchange(&s_got_ip, true)) {
        atomic_store(&s_ip_at_s, uptime_s());
        if (atomic_load(&s_up)) {
            ESP_LOGI(TAG, "on the network: the hotspot closes in %d s, for good", LINGER_S);
        }
    }
}

static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    // No client MACs in the log.
    ESP_LOGI(TAG, "a device %s the hotspot", id == WIFI_EVENT_AP_STACONNECTED ? "joined" : "left");
}

// The WPA2 passphrase: the serial from the device label, else random.
static void make_password(char pw[64])
{
    nvs_handle_t h;
    size_t len = 64;
    bool ok = nvs_open(BOARD_NVS_FACTORY_NS, NVS_READONLY, &h) == ESP_OK;
    if (ok) {
        ok = nvs_get_str(h, BOARD_NVS_MACHINE_SN, pw, &len) == ESP_OK && wifi_policy_ap_password_ok(pw);
        nvs_close(h);
    }
    s_random_pw = !ok;
    if (ok) {
        return;
    }
    static const char k_chars[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O, 1/l/I
    for (int i = 0; i < 12; i++) {
        pw[i] = k_chars[esp_random() % (sizeof(k_chars) - 1)];
    }
    pw[12] = '\0';
}

// Puts our AP config into the driver before esp_wifi_start, while nothing is
// on the air: the driver accepts an AP config only with the AP enabled, and
// the AP must never come up with its default (open) one.
static bool prepare(void)
{
    char pw[64];
    make_password(pw);
    if (s_random_pw) {
        // printf, not ESP_LOG: the console only, never the card log or its RAM ring.
        printf("\nsetup hotspot %s: no serial number in NVS; this boot's password is: %s\n\n", s_ssid, pw);
        fflush(stdout);
    }
    wifi_config_t cfg = {.ap = {.authmode = WIFI_AUTH_WPA2_PSK, .channel = 1, .max_connection = MAX_CLIENTS}};
    memcpy(cfg.ap.ssid, s_ssid, strlen(s_ssid));
    cfg.ap.ssid_len = strlen(s_ssid);
    memcpy(cfg.ap.password, pw, strlen(pw));
    mbedtls_platform_zeroize(pw, sizeof(pw));
    s_ap_netif = esp_netif_create_default_wifi_ap();  // 192.168.4.1/24 with a DHCP server
    esp_err_t err = s_ap_netif ? esp_wifi_set_mode(WIFI_MODE_APSTA) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
        esp_err_t back = esp_wifi_set_mode(WIFI_MODE_STA);
        err = err != ESP_OK ? err : back;
    }
    mbedtls_platform_zeroize(&cfg, sizeof(cfg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup hotspot unavailable: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

// What the driver runs on the AP is ours: WPA2-PSK, our name, a password.
static bool ap_config_ours(void)
{
    wifi_config_t c;
    bool ok = esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK && c.ap.authmode == WIFI_AUTH_WPA2_PSK &&
              c.ap.ssid_len == strlen(s_ssid) && !memcmp(c.ap.ssid, s_ssid, c.ap.ssid_len) && c.ap.password[0];
    mbedtls_platform_zeroize(&c, sizeof(c));
    return ok;
}

static void start_ap(void)
{
    atomic_store(&s_up, true);  // first: its clients are refused from the moment it exists
    wifi_mgr_op_begin();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK && !ap_config_ours()) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK && esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
        atomic_store(&s_up, false);
    }
    wifi_mgr_op_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hotspot failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "setup hotspot %s up: WPA2-PSK, password %s; page http://192.168.4.1/", s_ssid,
             s_random_pw ? "random (on the USB console)" : "the serial number on the device label");
}

static void stop_ap(const char *why)
{
    wifi_mgr_op_begin();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        atomic_store(&s_up, false);  // only once it is gone; else the next tick tries again
    }
    wifi_mgr_op_end();
    ESP_LOGI(TAG, "setup hotspot off (%s)%s", why, err == ESP_OK ? "" : esp_err_to_name(err));
}

// The station's network uses the hotspot's subnet: addresses would be ambiguous.
static bool subnet_clash(void)
{
    esp_netif_ip_info_t sta, ap;
    return esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &sta) == ESP_OK &&
           esp_netif_get_ip_info(s_ap_netif, &ap) == ESP_OK && sta.ip.addr &&
           (sta.ip.addr & ap.netmask.addr) == (ap.ip.addr & ap.netmask.addr);
}

static void watch_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t now = uptime_s(), forced = atomic_load(&s_forced_until_s);
        bool got_ip = atomic_load(&s_got_ip), up = atomic_load(&s_up);
        bool want;
        const char *why = "window over";
        if (got_ip) {
            want = up && now < atomic_load(&s_ip_at_s) + LINGER_S;
            why = "on the network";
        } else if (now < forced) {
            want = true;
        } else {
            wifi_mgr_status_t st;
            wifi_mgr_get_status(&st);
            want = wifi_policy_setup_ap_due(now, st.configured, false, WINDOW_S, UNREACHABLE_S);
        }
        if (up && !want && wifi_mgr_app_busy()) {
            want = true;  // a join from the setup page: let it finish and show its result
        }
        if (want && subnet_clash()) {
            want = false;
            why = "the network uses 192.168.4.0/24";
        }
        if (want && !up) {
            start_ap();
        } else if (!want && up) {
            stop_ap(why);
        }
    }
}

void wifi_setup_ap_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ssid, sizeof(s_ssid), "Makera_Z1_%02X%02X", mac[4], mac[5]);  // stock's name (discovery_fmt.h)
    if (!prepare()) {
        return;
    }
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL) != ESP_OK ||
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_ap_event, NULL) != ESP_OK ||
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_ap_event, NULL) != ESP_OK ||
        xTaskCreate(watch_task, "setup_ap", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "setup hotspot unavailable");
    }
}

bool wifi_mgr_setup_ap_active(void)
{
    return atomic_load(&s_up);
}

// The IPv4 address in sa: plain, or IPv4-mapped IPv6 (httpd's sockets are dual-stack).
static bool ipv4_of(const struct sockaddr_storage *sa, uint32_t *addr)
{
    if (sa->ss_family == AF_INET) {
        *addr = ((const struct sockaddr_in *)sa)->sin_addr.s_addr;
        return true;
    }
    const uint8_t *b = (const uint8_t *)&((const struct sockaddr_in6 *)sa)->sin6_addr;
    static const uint8_t k_mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (sa->ss_family == AF_INET6 && !memcmp(b, k_mapped, sizeof(k_mapped))) {
        memcpy(addr, b + 12, 4);
        return true;
    }
    return false;
}

// sa (native IPv6) is one of the hotspot's own IPv6 addresses.
static bool ap_ipv6(const struct sockaddr_storage *sa)
{
    esp_ip6_addr_t ip6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    int n = esp_netif_get_all_ip6(s_ap_netif, ip6);
    const uint8_t *b = (const uint8_t *)&((const struct sockaddr_in6 *)sa)->sin6_addr;
    for (int i = 0; i < n; i++) {
        if (!memcmp(ip6[i].addr, b, 16)) {
            return true;
        }
    }
    return false;
}

bool wifi_mgr_on_setup_ap(int fd)
{
    if (!atomic_load(&s_up)) {
        return false;
    }
    esp_netif_ip_info_t ap;
    struct sockaddr_storage local, peer;
    socklen_t ll = sizeof(local), pl = sizeof(peer);
    if (esp_netif_get_ip_info(s_ap_netif, &ap) != ESP_OK || getsockname(fd, (struct sockaddr *)&local, &ll) != 0 ||
        getpeername(fd, (struct sockaddr *)&peer, &pl) != 0) {
        return true;  // cannot tell while the hotspot runs: refuse
    }
    uint32_t l4, p4;
    if (!ipv4_of(&local, &l4) || !ipv4_of(&peer, &p4)) {
        return local.ss_family != AF_INET6 || ap_ipv6(&local);  // IPv6: only to the hotspot's own address
    }
    // IPv4: to the hotspot's address, or from its subnet to the station's
    // address (lwIP accepts a packet for any of its addresses on any
    // interface). The hotspot never runs while the station's network uses its
    // subnet (subnet_clash), so no LAN client matches.
    return l4 == ap.ip.addr || (p4 & ap.netmask.addr) == (ap.ip.addr & ap.netmask.addr);
}

void wifi_mgr_setup_ap_force(void)
{
    atomic_store(&s_got_ip, false);  // the next IP closes it again
    atomic_store(&s_forced_until_s, uptime_s() + WINDOW_S);
    ESP_LOGW(TAG, "setup hotspot window forced open for %d s", WINDOW_S);
}

void wifi_mgr_setup_ap_describe(char *out, size_t cap)
{
    if (!atomic_load(&s_up)) {
        snprintf(out, cap, "hotspot: off%s", atomic_load(&s_got_ip) ? " (on the network: closed for this boot)" : "");
        return;
    }
    wifi_config_t c;
    wifi_sta_list_t sta;
    unsigned n = esp_wifi_ap_get_sta_list(&sta) == ESP_OK ? (unsigned)sta.num : 0;
    esp_wifi_get_config(WIFI_IF_AP, &c);
    snprintf(out, cap, "hotspot: %s, %s, channel %u, %u device(s), password: %s", s_ssid,
             c.ap.authmode == WIFI_AUTH_WPA2_PSK ? "WPA2-PSK" : "NOT WPA2", c.ap.channel, n,
             s_random_pw ? "random (printed at start)" : "serial number");
    mbedtls_platform_zeroize(&c, sizeof(c));
}
