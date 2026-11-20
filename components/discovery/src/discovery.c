#include "discovery.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "discovery_fmt.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "machine.h"
#include "wifi_mgr.h"

// The official app listens for announcements on this UDP port (captured).
#define DISCOVERY_PORT 3333
// The state word while the LPC is not reporting (silent for MACHINE_STATE_STALE_MS
// or never heard). What stock sends then is unknown. No comma allowed.
#define DISCOVERY_STALE_STATE "Offline"

static const char *TAG = "discovery";
static unsigned s_tcp_port;
static bool (*s_busy)(void);

static void send_to(int sock, uint32_t addr_be, const char *msg, int len)
{
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(DISCOVERY_PORT),
        .sin_addr.s_addr = addr_be,
    };
    sendto(sock, msg, len, 0, (struct sockaddr *)&to, sizeof(to));
}

static _Atomic int s_sock = -1;  // stored last by discovery_start, loaded first by the tick
static char s_name[24];
static esp_netif_t *s_netif;
static bool s_announced;

void discovery_tick(void)
{
    int sock = atomic_load(&s_sock);
    esp_netif_ip_info_t ip;
    if (sock < 0 || !s_netif || esp_netif_get_ip_info(s_netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        s_announced = false;
        return;
    }
    // Nobody should send to this socket, but anything that does would sit
    // in lwIP's receive mailbox; throw it away.
    char junk[64];
    while (recv(sock, junk, sizeof(junk), MSG_DONTWAIT) > 0) {
    }

    char ip_str[16], state[MACHINE_STATE_MAX], msg[DISCOVERY_MAX_LEN + 1];
    esp_ip4addr_ntoa(&ip.ip, ip_str, sizeof(ip_str));
    if (!machine_get_state(state, sizeof(state))) {
        snprintf(state, sizeof(state), "%s", DISCOVERY_STALE_STATE);
    }
    int len = discovery_format(msg, sizeof(msg), s_name, ip_str, s_tcp_port, s_busy && s_busy(), state);
    if (len < 0) {
        return;
    }
    // A stock Z1 sends two datagrams per announcement (destinations not
    // captured). Sending to both the subnet-directed and the limited
    // broadcast covers either; on one L2 segment they are equivalent.
    send_to(sock, ip.ip.addr | ~ip.netmask.addr, msg, len);
    if (!wifi_mgr_setup_ap_active()) {  // the limited broadcast could leave on the hotspot
        send_to(sock, htonl(INADDR_BROADCAST), msg, len);
    }
    if (!s_announced) {
        ESP_LOGI(TAG, "announcing \"%.*s\" on UDP %d", len, msg, DISCOVERY_PORT);
        s_announced = true;
    }
}

esp_err_t discovery_start(unsigned tcp_port, bool (*busy)(void))
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return ESP_FAIL;
    }
    int on = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0) {
        close(sock);
        return ESP_FAIL;
    }
    s_tcp_port = tcp_port;
    s_busy = busy;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    discovery_name(s_name, sizeof(s_name), mac);
    s_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    atomic_store(&s_sock, sock);  // last: the tick reads the rest once it sees this
    return ESP_OK;
}
