#include "ctrl_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cfg.h"
#include "ctrl_priv.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "health.h"
#include "lpc_link.h"
#include "lpcfw.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include "ota.h"
#include "play.h"
#include "stats.h"
#include "stats_fmt.h"
#include "storage.h"
#include "wifi_mgr.h"

static const char *TAG = "ctrl";

#define VERSION_WAIT_US (5 * 1000 * 1000)
// A frame from the app arrives in well under this; a longer partial frame is
// bogus and would otherwise hold back the stop bytes queued behind it.
#define PARTIAL_FRAME_TIMEOUT_US (500 * 1000)
// Upload packets (8 KB) may take longer on a slow link; only they are this big.
#define PARTIAL_BIG_FRAME_TIMEOUT_US (3 * 1000 * 1000)
#define MAX_STATUS_DROPS 50
// Outgoing queue per client: an 8 KB download packet plus the status frames
// behind it. Above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB) it comes from
// PSRAM. Frames that do not fit are dropped whole for that client; one that
// keeps falling behind is disconnected.
#define CTRL_TXQ_SIZE 20480
// A client silent this long is closed: a live app polls status several times
// a second and pauses at most about 9 s between frames during a transfer.
#define CTRL_IDLE_TIMEOUT_S 15

// Stock's text when all client slots are taken (stock strings).
#define CLIENTS_FULL "The maximum number of client connections has been reached. Please close other client first."

// s_lock guards the slot table (fd, dead, order), every txq, the wake flag and
// the LPC version. The parser, rxbuf and timers belong to the server task.
static SemaphoreHandle_t s_lock;
client_t ctrl_clients[CONFIG_Z1_CTRL_MAX_CLIENTS];
static uint32_t s_next_order;
static char s_lpc_version[16];

// An eventfd the server's select() also waits on, so a frame queued from
// another task is sent at once instead of at the next select timeout.
static int s_wake_fd = -1;
static bool s_wake_pending;

void ctrl_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void ctrl_unlock(void) { xSemaphoreGive(s_lock); }

// Caller holds s_lock.
void ctrl_wake(void)
{
    if (s_wake_fd >= 0 && !s_wake_pending) {
        s_wake_pending = true;
        uint64_t one = 1;
        write(s_wake_fd, &one, sizeof(one));
    }
}

bool ctrl_client_is(int slot, uint32_t order)
{
    return ctrl_clients[slot].fd >= 0 && ctrl_clients[slot].order == order;
}

// Caller holds s_lock. A dropped status is harmless (the next one follows
// within a second); any other dropped frame (an "ok", LOAD_ERROR, FILE_*)
// breaks the app's flow, so the client is cut instead.
void ctrl_queue_frame(client_t *c, uint8_t type, const void *payload, size_t n)
{
    static uint8_t frame[FRAME_PAYLOAD_MAX + MK_FRAME_OVERHEAD];  // guarded by s_lock
    if (c->fd < 0 || c->dead) {
        return;
    }
    size_t len = mk_frame_encode(type, payload, n, frame, sizeof(frame));
    if (!len) {
        return;
    }
    if (ctrl_txq_put(&c->txq, frame, len)) {
        c->status_drops = 0;
        ctrl_wake();
        return;
    }
    if (type != PTYPE_STATUS_RES) {
        ESP_LOGW(TAG, "%s: output queue full; disconnecting", c->ip);
        c->dead = true;
    } else if (++c->status_drops >= MAX_STATUS_DROPS) {
        ESP_LOGW(TAG, "%s is not reading; disconnecting", c->ip);
        c->dead = true;
    }
}

void ctrl_reply_text(client_t *c, uint8_t type, const char *text)
{
    ctrl_lock();
    ctrl_queue_frame(c, type, text, strlen(text));
    ctrl_unlock();
}

// Caller holds s_lock and has checked that the LPC version is known.
static void send_version(client_t *c)
{
    char text[64];
    int n = ctrl_version_reply(text, sizeof(text), s_lpc_version, CONFIG_Z1_APP_ESP_VERSION);
    if (n > 0) {
        ctrl_queue_frame(c, PTYPE_NORMAL_INFO, text, (size_t)n);
    }
    c->version_deadline_us = 0;
}

void ctrl_refuse(client_t *c, const char *text, bool load_reply, bool file_cancel)
{
    ctrl_lock();
    ctrl_queue_frame(c, PTYPE_NORMAL_INFO, text, strlen(text));
    if (load_reply) {
        ctrl_queue_frame(c, PTYPE_LOAD_ERROR, NULL, 0);
    }
    if (file_cancel) {
        ctrl_queue_frame(c, PTYPE_FILE_CAN, NULL, 0);
    }
    ctrl_unlock();
}

// Stock answers a second client's transfer, and a client past the limit,
// with one 0x91 frame carrying the text, and nothing else (stock strings:
// "busy reply (0x91)").
void ctrl_busy_reply(client_t *c, const char *text)
{
    ctrl_reply_text(c, PTYPE_CLIENT_ERROR, text);
}

void ctrl_reject(client_t *c, const char *reason, bool load_reply, bool file_cancel)
{
    char text[160];
    snprintf(text, sizeof(text), "error: %s\n", reason);
    ctrl_refuse(c, text, load_reply, file_cancel);
}

// Caller holds s_lock. Sends what the socket takes right now.
static void send_some(client_t *c)
{
    if (c->dead) {
        return;
    }
    const uint8_t *p;
    size_t run = ctrl_txq_peek(&c->txq, &p);
    int sent = run ? send(c->fd, p, run, MSG_DONTWAIT) : 0;
    if (sent > 0) {
        ctrl_txq_consume(&c->txq, (size_t)sent);
    } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        c->dead = true;
    }
}

void ctrl_queue_big(client_t *c, uint8_t type, const void *payload, size_t n)
{
    int64_t give_up = esp_timer_get_time() + 3 * 1000 * 1000;
    ctrl_lock();
    while (!c->dead && c->fd >= 0 && c->txq.cap - c->txq.len < n + MK_FRAME_OVERHEAD) {
        send_some(c);
        ctrl_unlock();
        if (esp_timer_get_time() > give_up) {
            ctrl_lock();
            c->dead = true;  // not reading
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        ctrl_lock();
    }
    ctrl_queue_frame(c, type, payload, n);
    ctrl_unlock();
}

// ---- wlan: wifi_mgr runs it; the reply goes to the client that asked, if it
// is still there (usually not after joining another network).

static struct {
    int slot;
    uint32_t order;
    bool eof;  // -e: LOAD_INFO, then LOAD_FINISH/LOAD_ERROR
} s_wlan;

// wifi_mgr's task.
static void wlan_reply(void *ctx, const char *text, bool ok)
{
    ctrl_lock();
    if (ctrl_client_is(s_wlan.slot, s_wlan.order)) {
        client_t *c = &ctrl_clients[s_wlan.slot];
        if (text && text[0]) {
            ctrl_queue_frame(c, s_wlan.eof ? PTYPE_LOAD_INFO : PTYPE_NORMAL_INFO, text, strlen(text));
        }
        if (s_wlan.eof) {
            ctrl_queue_frame(c, ok ? PTYPE_LOAD_FINISH : PTYPE_LOAD_ERROR, ok ? "ok\r\n" : "", ok ? 4 : 0);
        }
    }
    ctrl_unlock();
}

static void wlan_command(client_t *c, const uint8_t *payload, const ctrl_route_t *r)
{
    wifi_app_cmd_t cmd = r->wlan_off ? WIFI_APP_OFF : r->nargs == 0 ? WIFI_APP_SCAN : WIFI_APP_JOIN;
    char ssid[WIFI_SSID_MAX + 1] = "", pass[WIFI_PASSWORD_MAX + 1] = "";
    if (cmd == WIFI_APP_JOIN &&
        (!ctrl_unescape(payload + r->arg_off[0], r->arg_len[0], ssid, sizeof(ssid)) ||
         (r->nargs > 1 && !ctrl_unescape(payload + r->arg_off[1], r->arg_len[1], pass, sizeof(pass))))) {
        mbedtls_platform_zeroize(pass, sizeof(pass));
        ctrl_reply_text(c, r->wlan_eof ? PTYPE_LOAD_INFO : PTYPE_NORMAL_INFO, "Error: Failed to set WiFi config\n");
        if (r->wlan_eof) {
            ctrl_reply_text(c, PTYPE_LOAD_ERROR, "");
        }
        return;
    }
    // Only this task starts commands, so a free wifi_mgr stays free until
    // the call below; the running command's reply never sees s_wlan change.
    bool started = false;
    if (!wifi_mgr_app_busy()) {
        s_wlan.slot = (int)(c - ctrl_clients);
        s_wlan.order = c->order;
        s_wlan.eof = r->wlan_eof;
        started = wifi_mgr_app_command(cmd, ssid, pass, wlan_reply, NULL);
    }
    mbedtls_platform_zeroize(pass, sizeof(pass));
    if (!started) {
        ctrl_reject(c, "busy", r->wlan_eof, false);
    }
}

// Every client may send every command, as stock; lines reach the LPC in the
// order they arrive.
static void handle_frame(client_t *c, uint8_t type, const uint8_t *payload, size_t n, ctrl_route_t r)
{
    switch (r.action) {
    case CTRL_IGNORE:
        break;
    case CTRL_FORWARD_REALTIME:
        // No reply when the LPC is unreachable: the app then shows its own
        // "no status" state, which is the truth.
        lpc_link_send(PTYPE_CTRL_SINGLE, payload, 1);
        break;
    case CTRL_FORWARD_LINE:
        if (r.track_play) {
            char path[XFER_PATH_MAX + 1];
            if (!xfer_path_decode((const char *)payload + r.path_off, r.path_len, path, sizeof(path))) {
                ctrl_reject(c, "invalid path", false, false);
                break;
            }
            // Arm before ota_active(): see s_uploads.
            play_job_arm(path, xfer_path_lpc_crc((const char *)payload + r.path_off, r.path_len));
            if (ota_active() || lpcfw_busy()) {
                ctrl_reject(c, "busy updating", false, false);
                break;
            }
        }
        if (lpc_link_send(PTYPE_CTRL_MULTI, payload + r.line_off, r.line_len) != ESP_OK) {
            ctrl_reject(c, "motion controller not connected", false, false);
        }
        break;
    case CTRL_LOCAL_VERSION:
        ctrl_lock();
        if (s_lpc_version[0]) {
            send_version(c);
        } else {
            // The LPC announces its version every 5 s; wait for it.
            c->version_deadline_us = esp_timer_get_time() + VERSION_WAIT_US;
        }
        ctrl_unlock();
        break;
    case CTRL_LOCAL_TIME: {
        const char *t = (const char *)payload + r.line_off;
        if (r.line_len == 4) {
            char text[40];
            snprintf(text, sizeof(text), "time = %lld\r\n", (long long)time(NULL));  // stock's reply
            ctrl_reply_text(c, PTYPE_NORMAL_INFO, text);
        } else {
            int64_t clock;
            if (stats_parse_time(t, r.line_len, &clock)) {
                stats_on_line(t, r.line_len);  // sets the clock (and the first boot, once)
            } else {
                ctrl_reply_text(c, PTYPE_NORMAL_INFO, "Invalid parameter length of time command!\r\n");  // stock's text
            }
        }
        break;
    }
    case CTRL_LOCAL_SN:
    case CTRL_LOCAL_SYS_TIME: {
        char text[96];
        int len = r.action == CTRL_LOCAL_SN ? stats_sn_reply(text, sizeof(text))
                                            : stats_sys_time_reply(text, sizeof(text));
        if (len > 0) {
            ctrl_reply_text(c, PTYPE_NORMAL_INFO, text);
        }
        break;
    }
    case CTRL_LOCAL_M485:
        ctrl_reply_text(c, PTYPE_NORMAL_INFO, "current communication protocol: makera\n");
        break;
    case CTRL_REJECT:
        ctrl_reject(c, r.reason, r.load_reply, r.file_cancel);
        break;
    case CTRL_LOCAL_CONFIG: {
        char cmd[16], args[3][72], text[200];
        const char *a[3] = {NULL, NULL, NULL};
        snprintf(cmd, sizeof(cmd), "%.*s", (int)r.line_len, (const char *)payload + r.line_off);
        for (size_t i = 0; i < r.nargs; i++) {
            snprintf(args[i], sizeof(args[i]), "%.*s", (int)r.arg_len[i], (const char *)payload + r.arg_off[i]);
            a[i] = args[i];
        }
        cfg_command(cmd, a[0], a[1], a[2], text, sizeof(text));
        ctrl_reply_text(c, PTYPE_NORMAL_INFO, text);
        break;
    }
    case CTRL_LOCAL_WLAN:
        wlan_command(c, payload, &r);
        break;
    case CTRL_LOCAL_FTYPE:
        // Stock answers this itself (the LPC would say "lz"): the app then
        // uploads plain files, with no QuickLZ on the machine.
        ctrl_reply_text(c, PTYPE_NORMAL_INFO, "ftype = nc\n");
        break;
    case CTRL_LOCAL_LS:
        ctrl_files_ls(c, payload, &r);
        break;
    case CTRL_LOCAL_RM:
    case CTRL_LOCAL_MV:
    case CTRL_LOCAL_MKDIR:
        ctrl_files_op(c, payload, &r);
        break;
    case CTRL_FILE_UPLOAD:
        ctrl_files_upload(c, payload + r.line_off, r.line_len);
        break;
    case CTRL_FILE_DOWNLOAD:
        ctrl_files_download(c, payload + r.line_off, r.line_len);
        break;
    case CTRL_FILE_FRAME:
        ctrl_files_frame(c, type, payload, n);
        break;
    case CTRL_LOCAL_PLAYING:
        ctrl_files_playing(c);
        break;
    case CTRL_LOCAL_RESET:
        if (!lpcfw_bundle_present()) {
            // A client's own `reset`: the LPC restarts (and the machine homes).
            const char *note = lpcfw_reset_warning();
            ctrl_server_on_lpc_frame(PTYPE_NORMAL_INFO, (const uint8_t *)note, strlen(note));
            if (lpc_link_send(PTYPE_CTRL_MULTI, payload + r.line_off, r.line_len) != ESP_OK) {
                ctrl_reject(c, "motion controller not connected", false, false);
            }
            break;
        }
        // As stock, `reset` installs a waiting package.
        __attribute__((fallthrough));
    case CTRL_LOCAL_UPGRADE_CANCEL: {
        const char *why = lpcfw_cancel();
        if (why) {
            ctrl_reply_text(c, PTYPE_NORMAL_INFO, why);
        }
        break;
    }
    case CTRL_LOCAL_UPGRADE: {
        // An upload may be replacing the package right now.
        const char *why = ctrl_server_uploading() ? "Error: an upload is running; try again afterwards.\r\n"
                                                  : lpcfw_upgrade();  // results reach every client later
        if (why) {
            ctrl_reply_text(c, PTYPE_NORMAL_INFO, why);
        }
        break;
    }
    }
}

static void on_client_frame(uint8_t type, const uint8_t *payload, size_t n, void *ctx)
{
    client_t *c = ctx;
    c->partial_since_us = 0;  // a frame completed; the timeout covers the next one only
    ctrl_route_t r = ctrl_route(type, payload, n);
    handle_frame(c, type, payload, n, r);
    if (r.secret) {
        // A wlan line may hold a password, whatever became of it (refused,
        // busy, wrong arguments): don't leave it in the receive buffer.
        mbedtls_platform_zeroize((void *)payload, n);
    }
}

void ctrl_server_lpc_version(char *out, size_t cap)
{
    if (!s_lock) {
        snprintf(out, cap, "%s", "");
        return;
    }
    ctrl_lock();
    snprintf(out, cap, "%s", s_lpc_version);
    ctrl_unlock();
}

void ctrl_server_on_lpc_frame(uint8_t type, const uint8_t *payload, size_t n)
{
    if (!s_lock) {
        return;
    }
    if (type == PTYPE_FIRM_VER) {
        size_t len = 0;
        while (len < n && ((payload[len] >= '0' && payload[len] <= '9') || payload[len] == '.')) {
            len++;
        }
        if (len == 0 || len >= sizeof(s_lpc_version)) {
            return;
        }
        ctrl_lock();
        memcpy(s_lpc_version, payload, len);
        s_lpc_version[len] = '\0';
        for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS; i++) {
            if (ctrl_clients[i].fd >= 0 && ctrl_clients[i].version_deadline_us) {
                send_version(&ctrl_clients[i]);
            }
        }
        ctrl_unlock();
        return;
    }

    wifi_mgr_status_t wst = {0};
    if (type == PTYPE_DIAG_RES) {
        wifi_mgr_get_status(&wst);  // before s_lock: it takes wifi_mgr's own lock
    }
    uint32_t sd_used = 0, sd_total = 0;
    storage_space_mb(&sd_used, &sd_total);
    ctrl_lock();
    // Everything the LPC says goes to every client, as stock: its status
    // (answering anyone's `?`), its console output and its listings. Local
    // replies (version, ls, config-*, wlan, sn-get, sys-time, transfers) go
    // to the asking client only, elsewhere.
    static char buf[LPC_MAX_RX_PAYLOAD + 64];  // under s_lock; the LPC sends at most LPC_MAX_RX_PAYLOAD
    int m = -1;
    if (type == PTYPE_STATUS_RES) {
        unsigned phase, pct;
        lpcfw_ota_report(&phase, &pct);
        m = ctrl_status_extras(buf, sizeof(buf), payload, n, sd_used, sd_total, phase, pct);  // stock's E and OTA
    } else if (type == PTYPE_DIAG_RES) {
        m = wst.connected ? ctrl_diag_rssi(buf, sizeof(buf), payload, n, wst.rssi) : -1;  // stock's RSSI
    } else if (type != PTYPE_NORMAL_INFO && type != PTYPE_LOAD_INFO && type != PTYPE_LOAD_FINISH &&
               type != PTYPE_LOAD_ERROR) {
        ctrl_unlock();
        return;
    }
    for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS; i++) {
        if (ctrl_clients[i].fd >= 0) {
            ctrl_queue_frame(&ctrl_clients[i], type, m > 0 ? (const uint8_t *)buf : payload, m > 0 ? (size_t)m : n);
        }
    }
    ctrl_unlock();
}

bool ctrl_server_has_clients(void)
{
    if (!s_lock) {
        return false;
    }
    bool any = false;
    ctrl_lock();
    for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS && !any; i++) {
        any = ctrl_clients[i].fd >= 0 && !ctrl_clients[i].dead;
    }
    ctrl_unlock();
    return any;
}

void ctrl_server_dump(void)
{
    char lpc[64];
    cfg_lpc_status(lpc, sizeof(lpc));
    printf("lpc_config: %s\n", lpc);
    if (!s_lock) {
        printf("ctrl: not started\n");
        return;
    }
    ctrl_lock();
    printf("ctrl: lpc version '%s'\n", s_lpc_version);
    for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS; i++) {
        client_t *c = &ctrl_clients[i];
        if (c->fd < 0) {
            continue;
        }
        printf("  client %s%s: queued %u/%u bytes, bad frames %lu, bad CRCs %lu%s\n", c->ip,
               c->dead ? " (closing)" : "", (unsigned)c->txq.len, (unsigned)c->txq.cap, (unsigned long)c->parser.bad_frame,
               (unsigned long)c->parser.bad_crc, c->dl.state != XFER_DL_IDLE ? ", downloading" : "");
    }
    ctrl_unlock();
}

static void accept_client(int listen_fd)
{
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    int fd = accept(listen_fd, (struct sockaddr *)&from, &flen);
    if (fd < 0) {
        return;
    }
    if (wifi_mgr_on_setup_ap(fd)) {
        close(fd);  // the setup hotspot serves only its setup page
        ESP_LOGW(TAG, "refused a client from the setup hotspot");
        return;
    }
    char ip[16];
    inet_ntoa_r(from.sin_addr, ip, sizeof(ip));

    ctrl_lock();
    client_t *c = NULL;
    for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS && !c; i++) {
        if (ctrl_clients[i].fd < 0) {
            c = &ctrl_clients[i];
        }
    }
    if (c) {
        c->fd = fd;
        c->dead = false;
        c->order = s_next_order++;
        c->last_rx_us = esp_timer_get_time();
        c->partial_since_us = 0;
        c->version_deadline_us = 0;
        c->status_drops = 0;
        c->dl.state = XFER_DL_IDLE;
        c->ul.state = XFER_UL_IDLE;
        c->ul_tmp[0] = '\0';
        c->xfile = NULL;
        snprintf(c->ip, sizeof(c->ip), "%s", ip);
        ctrl_txq_init(&c->txq, c->txbuf, CTRL_TXQ_SIZE);
        mk_parser_init(&c->parser, c->rxbuf, sizeof(c->rxbuf), MK_RELIABLE);
    }
    ctrl_unlock();

    if (!c) {
        // Stock's text, as one frame on the fresh socket, then close it.
        static const char k_full[] = CLIENTS_FULL;  // 91 bytes, no newline (stock)
        uint8_t f[sizeof(k_full) + MK_FRAME_OVERHEAD];
        size_t len = mk_frame_encode(PTYPE_CLIENT_ERROR, k_full, sizeof(k_full) - 1, f, sizeof(f));
        if (len) {
            send(fd, f, len, MSG_DONTWAIT);
        }
        ESP_LOGW(TAG, "refusing %s: %d clients already connected", ip, CONFIG_Z1_CTRL_MAX_CLIENTS);
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    ESP_LOGI(TAG, "client %s connected", ip);
}

static void close_client(client_t *c)
{
    ctrl_files_close(c);
    ctrl_lock();
    close(c->fd);
    c->fd = -1;
    c->dead = false;
    ctrl_unlock();
    ESP_LOGI(TAG, "client %s disconnected", c->ip);
}

static void server_task(void *arg)
{
    int listen_fd = (int)(intptr_t)arg;
    health_watch(HEALTH_CTRL, false);  // waits on the card: no watchdog
    for (;;) {
        health_beat(HEALTH_CTRL);
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(listen_fd, &rd);
        int maxfd = listen_fd;
        if (s_wake_fd >= 0) {
            FD_SET(s_wake_fd, &rd);
            maxfd = s_wake_fd > maxfd ? s_wake_fd : maxfd;
        }
        ctrl_lock();
        // Frames queued from now on send a new wake-up.
        s_wake_pending = false;
        for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS; i++) {
            client_t *c = &ctrl_clients[i];
            if (c->fd < 0) {
                continue;
            }
            FD_SET(c->fd, &rd);
            if (c->txq.len) {
                FD_SET(c->fd, &wr);
            }
            maxfd = c->fd > maxfd ? c->fd : maxfd;
        }
        ctrl_unlock();

        struct timeval tv = {.tv_sec = 0, .tv_usec = 100 * 1000};
        if (select(maxfd + 1, &rd, &wr, NULL, &tv) < 0) {
            ESP_LOGW(TAG, "select failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s_wake_fd >= 0 && FD_ISSET(s_wake_fd, &rd)) {
            uint64_t n;
            read(s_wake_fd, &n, sizeof(n));  // readable, so it does not block; clears the count
        }
        if (FD_ISSET(listen_fd, &rd)) {
            accept_client(listen_fd);
        }

        int64_t now = esp_timer_get_time();
        // First read every client and pass on its stop bytes (feed hold, halt,
        // jog stop) at once, ahead of anything slow another frame may cause
        // (an upload packet's SD write, a listing). The parser later forwards
        // them once more, which changes nothing: they are idempotent.
        static uint8_t rx[CONFIG_Z1_CTRL_MAX_CLIENTS][512];
        int rxn[CONFIG_Z1_CTRL_MAX_CLIENTS];
        for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS; i++) {
            client_t *c = &ctrl_clients[i];
            rxn[i] = c->fd >= 0 && FD_ISSET(c->fd, &rd) ? recv(c->fd, rx[i], sizeof(rx[i]), 0) : -2;
            uint8_t stops[16];
            size_t k = rxn[i] > 0 ? ctrl_stop_bytes(rx[i], (size_t)rxn[i], stops, sizeof(stops)) : 0;
            for (size_t j = 0; j < k; j++) {
                lpc_link_send(PTYPE_CTRL_SINGLE, &stops[j], 1);
            }
        }
        ctrl_files_poll(now);
        for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS; i++) {
            client_t *c = &ctrl_clients[i];
            if (c->fd < 0) {
                continue;
            }
            if (rxn[i] != -2) {
                int n = rxn[i];
                uint8_t *buf = rx[i];
                if (n > 0) {
                    c->last_rx_us = now;
                    mk_parser_feed(&c->parser, buf, (size_t)n, on_client_frame, c);
                    mbedtls_platform_zeroize(buf, (size_t)n);  // it may have carried a password
                    if (!mk_parser_busy(&c->parser)) {
                        c->partial_since_us = 0;
                    } else if (!c->partial_since_us) {
                        c->partial_since_us = now;
                    }
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    c->dead = true;
                }
            }
            int64_t partial_limit = c->parser.need > 1024 ? PARTIAL_BIG_FRAME_TIMEOUT_US : PARTIAL_FRAME_TIMEOUT_US;
            if (c->partial_since_us && now - c->partial_since_us > partial_limit) {
                ESP_LOGW(TAG, "%s: dropping a frame that never completed", c->ip);
                mk_parser_drop_partial(&c->parser);
                c->partial_since_us = 0;
            }

            ctrl_lock();
            if (FD_ISSET(c->fd, &wr)) {
                send_some(c);
            }
            bool version_timed_out = c->version_deadline_us && now >= c->version_deadline_us;
            if (version_timed_out) {
                // Answering with a made-up version would make the app's update
                // check misjudge the machine; no answer is the honest one.
                c->version_deadline_us = 0;
            }
            bool idle = now - c->last_rx_us > (int64_t)CTRL_IDLE_TIMEOUT_S * 1000000;
            bool dead = c->dead || idle;
            ctrl_unlock();
            if (version_timed_out) {
                ESP_LOGW(TAG, "%s asked for the version, but the LPC has not reported one", c->ip);
            }
            if (idle && !c->dead) {
                ESP_LOGI(TAG, "client %s idle for %d s", c->ip, CTRL_IDLE_TIMEOUT_S);
            }
            if (dead) {
                close_client(c);
            }
        }
    }
}

static void open_wake_fd(void)
{
    const esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    esp_err_t err = esp_vfs_eventfd_register(&cfg);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {  // INVALID_STATE: registered already
        s_wake_fd = eventfd(0, 0);
    }
    if (s_wake_fd < 0) {
        ESP_LOGW(TAG, "no wake-up eventfd; replies may wait up to 100 ms");
    }
}

esp_err_t ctrl_server_start(void)
{
    if (s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctrl_files_start() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS; i++) {
        ctrl_clients[i].fd = -1;
        ctrl_clients[i].txbuf = malloc(CTRL_TXQ_SIZE);
        if (!ctrl_clients[i].txbuf) {
            while (i--) {
                free(ctrl_clients[i].txbuf);
                ctrl_clients[i].txbuf = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    open_wake_fd();

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return ESP_FAIL;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CTRL_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        return ESP_FAIL;
    }
    if (xTaskCreate(server_task, "ctrl_server", 6144, (void *)(intptr_t)fd, 6, NULL) != pdPASS) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "listening on TCP %d (max %d clients)", CTRL_PORT, CONFIG_Z1_CTRL_MAX_CLIENTS);
    return ESP_OK;
}
