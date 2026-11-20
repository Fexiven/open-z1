#include "lpcfw.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lpc_link.h"
#include "lpcfw_bundle.h"
#include "lpcfw_sm.h"
#include "machine.h"
#include "mkframe.h"
#include "ota.h"
#include "play.h"
#include "storage.h"
#include "xfer_fs.h"

static const char *TAG = "lpcfw";

#define IMAGE_TMP "/sd/.lpc1768.bin.part"
// Stock's texts.
#define FORMAT_ERROR "Error: The firmware file format is incorrect or it has been damaged. Please re-upload\r\n"
#define DELETE_ERROR "Error: failed to delete [%s], please delete manually.\r\n"

// After `reset\n` the LPC restarts in 3 s (SimpleShell::reset_command) and its
// bootloader asks at once (stock: 3.6 s after the reset).
#define AWAIT_BOOTLOADER_MS 10000
#define SHOW_DONE_MS 3000  // stock's latch of |OTA:4,100
#define SHOW_FAILED_MS 10000

enum { OTA_IDLE = 0, OTA_LPC = 2, OTA_ERROR = 3, OTA_SUCCESS = 4 };

typedef struct {
    uint8_t kind;  // MSG_*
    lpcfw_ev_t ev;  // MSG_EVENT: what the exchange did, with the state right after it
    const char *why;
    uint32_t next, last, total;
    bool app;  // the event came from a frame of the LPC firmware (it runs)
} msg_t;
enum { MSG_UPGRADE, MSG_CANCEL, MSG_EVENT };

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;  // s_sm, s_ram, s_image
static void (*s_notify)(const char *text);
static atomic_bool s_claim;     // `upgrade` or a cancel is being decided (a lock, taken by exchange)
static atomic_bool s_await;     // `reset\n` sent on a client's command, waiting for the bootloader
static atomic_bool s_session;   // serving the bootloader (s_sm active)
static atomic_uint s_report;    // phase << 8 | pct

// Under s_lock. The image is served from RAM, right in the LPC's receive
// task: no card access between a request and its answer.
static lpcfw_sm_t s_sm;
static uint8_t *s_ram;     // /sd/lpc1768.bin, NULL: none (nothing is served)
static uint32_t s_image;   // its size
static int64_t s_rx_us;    // the last request, for the answer's latency in the log

// lpcfw_task only.
static int64_t s_await_until, s_show_until;
static uint8_t s_buf[4096];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void notify(const char *text)
{
    ESP_LOGD(TAG, "to the app: %.*s", (int)strcspn(text, "\r\n"), text);
    if (s_notify) {
        s_notify(text);
    }
}

static void report(unsigned phase, unsigned pct)
{
    atomic_store(&s_report, phase << 8 | (pct & 0xFF));
}

void lpcfw_set_notify(void (*fn)(const char *text))
{
    s_notify = fn;
}

void lpcfw_ota_report(unsigned *phase, unsigned *pct)
{
    unsigned r = atomic_load(&s_report);
    *phase = r >> 8;
    *pct = r & 0xFF;
}

bool lpcfw_busy(void)
{
    return atomic_load(&s_claim) || atomic_load(&s_await) || atomic_load(&s_session);
}

bool lpcfw_bundle_present(void)
{
    struct stat st;
    if (!storage_hold()) {
        return false;
    }
    bool ok = stat(LPCFW_BUNDLE, &st) == 0 && S_ISREG(st.st_mode);
    storage_release();
    return ok;
}

// Why the LPC must not be reset now, or NULL.
static const char *not_ready(void)
{
    if (ota_active()) {
        return "Error: the Mainboard is installing an update; try again afterwards.\r\n";
    }
    if (play_job_busy()) {
        return "Error: a job is running; firmware update refused.\r\n";
    }
    char st[MACHINE_STATE_MAX];
    if (!machine_get_state(st, sizeof(st))) {
        return "Error: the motion controller does not answer; firmware update refused.\r\n";
    }
    if (strcmp(st, "Idle") != 0) {
        return "Error: the machine is not idle; firmware update refused.\r\n";
    }
    return NULL;
}

// Reads /sd/lpc1768.bin into RAM (PSRAM if there is). NULL on success, else why not.
static const char *image_load(uint8_t **ram, uint32_t *size)
{
    struct stat st;
    *ram = NULL;
    if (!storage_hold()) {
        return "no SD card";
    }
    const char *bad = NULL;
    FILE *f = NULL;
    if (stat(LPCFW_IMAGE, &st) != 0) {
        bad = "absent";
    } else if (!S_ISREG(st.st_mode) || st.st_size < 8 || st.st_size > LPCFW_LPC_MAX) {
        bad = "wrong size";
    } else if (!(f = fopen(LPCFW_IMAGE, "rb"))) {
        bad = "unreadable";
    } else {
        *size = (uint32_t)st.st_size;
        *ram = heap_caps_malloc(*size, MALLOC_CAP_SPIRAM);
        if (!*ram) {
            *ram = malloc(*size);
        }
        if (!*ram) {
            bad = "no memory for it";
        } else if (fread(*ram, 1, *size, f) != *size) {
            bad = "unreadable";
        } else {
            bad = lpcfw_lpc_vectors_ok(*ram, *size);
        }
    }
    if (f) {
        fclose(f);
    }
    storage_release();
    if (bad && *ram) {
        free(*ram);
        *ram = NULL;
    }
    if (!bad) {
        ESP_LOGI(TAG, "%s loaded: %lu bytes, CRC32 %08lx", LPCFW_IMAGE, (unsigned long)*size,
                 (unsigned long)lpcfw_crc32(0, *ram, *size));
    }
    return bad;
}

static void image_set(uint8_t *ram, uint32_t size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_ram);
    s_ram = ram;
    s_image = ram ? size : 0;
    xSemaphoreGive(s_lock);
}

// Deletes /sd/lpc1768.bin and its copy in RAM; notifies if it stays.
static bool image_drop(void)
{
    image_set(NULL, 0);
    bool held = storage_hold();
    struct stat st;
    bool gone = held && (stat(LPCFW_IMAGE, &st) != 0 || remove(LPCFW_IMAGE) == 0);
    if (held) {
        storage_release();
    }
    if (!gone) {
        char text[120];
        snprintf(text, sizeof(text), DELETE_ERROR, LPCFW_IMAGE);
        notify(text);
    }
    return gone;
}

static void hex(char *out, size_t cap, const uint8_t *p, size_t n)
{
    size_t k = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n && i < 8 && k + 3 < cap; i++) {
        k += (size_t)snprintf(out + k, cap - k, "%02x", p[i]);
    }
}

static void post(const msg_t *m)
{
    if (!s_queue || xQueueSend(s_queue, m, 0) != pdTRUE) {
        ESP_LOGE(TAG, "event %d lost", (int)m->ev);
    }
}

// Under s_lock: sends the answer o asks for, and hands the event to the task.

static void handle_out(lpcfw_out_t o, bool app)
{
    if (o.type) {
        static uint8_t frame[LPC_FIRM_TX_PAYLOAD];
        memcpy(frame, o.head, o.head_len);
        if (o.data_len) {
            memcpy(frame + o.head_len, s_ram + o.data_off, o.data_len);
        }
        size_t n = o.head_len + o.data_len;
        esp_err_t err = lpc_link_send(o.type, frame, n);
        char h[20];
        hex(h, sizeof(h), frame, n);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "FIRM tx %02X len %u [%s] NOT SENT: %s", o.type, (unsigned)n, h, esp_err_to_name(err));
        } else {
            ESP_LOGD(TAG, "FIRM tx %02X len %u [%s] %lld us after the request", o.type, (unsigned)n, h,
                     esp_timer_get_time() - s_rx_us);
        }
    }
    switch (o.ev) {
    case LPCFW_EV_NONE:
        return;
    case LPCFW_EV_STARTED:
        if (!atomic_exchange(&s_session, true)) {
            lpc_link_console_block(true);
        }
        atomic_store(&s_await, false);
        report(OTA_LPC, s_sm.pct);
        break;
    case LPCFW_EV_PROGRESS:
        report(OTA_LPC, s_sm.pct);
        break;
    case LPCFW_EV_DONE:
    case LPCFW_EV_FAILED:
        atomic_store(&s_session, false);
        lpc_link_console_block(false);
        break;
    default:
        break;
    }
    msg_t m = {.kind = MSG_EVENT, .ev = o.ev, .why = o.why, .next = s_sm.next, .last = s_sm.last,
               .total = s_sm.total, .app = app};
    if (o.ev != LPCFW_EV_PROGRESS || s_sm.pct % 10 == 0) {
        post(&m);
    }
}

static void on_event(const msg_t *m)
{
    char text[260];
    switch (m->ev) {
    case LPCFW_EV_DECLINED:
        ESP_LOGI(TAG, "LPC bootloader: no firmware update");
        break;
    case LPCFW_EV_STARTED:
        ESP_LOGI(TAG, "LPC bootloader: serving %s (%lu frames from %lu)", LPCFW_IMAGE, (unsigned long)m->total,
                 (unsigned long)m->next + 1);
        s_await_until = 0;
        s_show_until = 0;
        break;
    case LPCFW_EV_PROGRESS:
        ESP_LOGI(TAG, "LPC firmware: frame %lu of %lu", (unsigned long)m->last, (unsigned long)m->total);
        break;
    case LPCFW_EV_DONE:
        ESP_LOGI(TAG, "LPC firmware installed (%lu frames)", (unsigned long)m->total);
        image_drop();
        report(OTA_SUCCESS, 100);
        s_show_until = now_ms() + SHOW_DONE_MS;
        notify("Info: motion controller firmware updated.\r\n");
        break;
    case LPCFW_EV_FAILED:
        report(OTA_ERROR, (unsigned)((uint64_t)m->next * 100 / (m->total ? m->total : 1)));
        s_show_until = now_ms() + SHOW_FAILED_MS;
        if (m->app && m->next <= 1) {
            // The LPC firmware runs and the bootloader took no frame past the
            // first: nothing was replaced, so nothing needs recovering. The
            // image is not offered again (no retries) until the next `upgrade`.
            bool gone = image_drop();
            snprintf(text, sizeof(text),
                     "Error: the motion controller did not take the update (%s, after frame %lu of %lu); it "
                     "kept its firmware.%s Upload the package again to retry.\r\n",
                     m->why, (unsigned long)m->last, (unsigned long)m->total,
                     gone ? " " LPCFW_IMAGE " removed." : "");
        } else {
            snprintf(text, sizeof(text),
                     "Error: motion controller update stopped at frame %lu of %lu (%s). " LPCFW_IMAGE
                     " stays on the card and is served when the motion controller asks for it at its next "
                     "start (switch the machine off and on); `upgrade cancel` removes it.\r\n",
                     (unsigned long)m->last, (unsigned long)m->total, m->why);
        }
        notify(text);
        break;
    case LPCFW_EV_IGNORED:
        ESP_LOGW(TAG, "LPC bootloader: %s, not answered", m->why);
        break;
    default:
        break;
    }
}

// Takes the LPC image out of the bundle into /sd/lpc1768.bin. NULL on
// success (*size, ver set), else the text for the app. The bundle is deleted
// once opened, whatever the outcome.
static const char *extract(uint32_t *size, char *ver, size_t ver_cap)
{
    static char text[160];
    if (storage_read_only()) {
        return "Error: the SD card is read-only in this firmware\r\n";
    }
    if (!storage_hold()) {
        return "Error: no SD card\r\n";
    }
    xfer_fs_recover(LPCFW_BUNDLE);
    FILE *in = fopen(LPCFW_BUNDLE, "rb");
    if (!in) {
        storage_release();
        return "Error: no firmware package (" LPCFW_BUNDLE "); upload it first\r\n";
    }
    struct stat st;
    uint8_t hdr[LPCFW_HDR_LEN];
    lpcfw_hdr_t h;
    const char *bad = fstat(fileno(in), &st) != 0 || fread(hdr, 1, sizeof(hdr), in) != sizeof(hdr)
                          ? "unreadable"
                          : lpcfw_hdr_parse(hdr, sizeof(hdr), (uint64_t)st.st_size, &h);
    const char *reply = bad ? FORMAT_ERROR : NULL;
    if (!bad && (bad = lpcfw_hdr_lpc_ok(&h)) != NULL) {
        reply = h.flags & LPCFW_FLAG_LPC
                    ? FORMAT_ERROR
                    : "Error: this package has no motion-controller firmware (the open Mainboard firmware never "
                      "installs Makera's Mainboard firmware).\r\n";
    }
    FILE *out = NULL;
    if (!bad && !(out = fopen(IMAGE_TMP, "wb"))) {
        bad = "cannot create " IMAGE_TMP;
        reply = "Error: cannot write " LPCFW_IMAGE "\r\n";
    }
    uint32_t lpc_crc = 0, written = 0;
    uint8_t first[8] = {0};
    if (!bad) {
        // One pass: the file CRC over everything but its own field, and the
        // LPC image copied out as it goes by.
        uint32_t crc = lpcfw_crc32(0, hdr, 28);
        uint64_t pos = LPCFW_HDR_LEN;
        bool wrote = true;
        size_t k;
        while ((k = fread(s_buf, 1, sizeof(s_buf), in)) > 0) {
            crc = lpcfw_crc32(crc, s_buf, k);
            size_t off, m = lpcfw_lpc_slice(&h, pos, k, &off);
            for (size_t i = 0; i < m && written + i < 8; i++) {
                first[written + i] = s_buf[off + i];
            }
            lpc_crc = lpcfw_crc32(lpc_crc, s_buf + off, m);
            wrote &= fwrite(s_buf + off, 1, m, out) == m;
            written += (uint32_t)m;
            pos += k;
        }
        wrote &= fflush(out) == 0 && fsync(fileno(out)) == 0;
        wrote &= fclose(out) == 0;
        if (ferror(in) || pos != (uint64_t)st.st_size || written != h.lpc_size) {
            bad = "read error";
        } else if (crc != h.file_crc) {
            bad = "file CRC mismatch";
        } else {
            bad = lpcfw_lpc_vectors_ok(first, h.lpc_size);
        }
        reply = bad ? FORMAT_ERROR : NULL;  // the package's fault; from here on the card's
        if (!bad && !wrote) {
            bad = "write error";
        }
        if (!bad) {
            // Read back what the card holds now.
            FILE *chk = fopen(IMAGE_TMP, "rb");
            uint32_t c = 0, got = 0;
            while (chk && (k = fread(s_buf, 1, sizeof(s_buf), chk)) > 0) {
                c = lpcfw_crc32(c, s_buf, k);
                got += (uint32_t)k;
            }
            if (chk) {
                fclose(chk);
            }
            if (c != lpc_crc || got != h.lpc_size) {
                bad = "read-back mismatch";
            } else if (!xfer_fs_keep(IMAGE_TMP, LPCFW_IMAGE, NULL)) {
                bad = "cannot rename it";
            }
        }
        if (bad && !reply) {
            reply = "Error: cannot write " LPCFW_IMAGE "\r\n";
        }
        if (bad) {
            remove(IMAGE_TMP);
        }
    }
    fclose(in);
    if (bad) {
        ESP_LOGW(TAG, "%s refused: %s", LPCFW_BUNDLE, bad);
    } else {
        *size = h.lpc_size;
        lpcfw_version_str(h.lpc_ver, ver, ver_cap);
        char esp[16];
        lpcfw_version_str(h.esp_ver, esp, sizeof(esp));
        ESP_LOGI(TAG, "%s: LPC1768 %s (%lu bytes, CRC32 %08lx) kept as %s; its ESP32 part (%s) ignored", LPCFW_BUNDLE,
                 ver, (unsigned long)h.lpc_size, (unsigned long)lpc_crc, LPCFW_IMAGE,
                 h.flags & LPCFW_FLAG_ESP ? esp : "none");
    }
    if (!xfer_fs_rm(LPCFW_BUNDLE)) {
        snprintf(text, sizeof(text), DELETE_ERROR, LPCFW_BUNDLE);
        notify(text);
    }
    storage_release();
    return bad ? reply : NULL;
}


// A Z1 homes after every LPC restart.
#define RESET_WARNING "Info: The motion controller restarts and the machine will home.\r\n"

// `reset\n` to the LPC firmware, as stock: its bootloader then asks. The only
// place this firmware resets the LPC, and only from do_upgrade (a client's
// `upgrade`/`reset`), after not_ready() passed.
static bool reset_lpc_on_command(void)
{
    atomic_store(&s_await, true);  // busy before the LPC hears it
    notify(RESET_WARNING);
    if (lpc_link_send(PTYPE_CTRL_MULTI, "reset\n", 6) != ESP_OK) {
        atomic_store(&s_await, false);
        return false;
    }
    lpc_link_console_block(true);  // from now on only the bootloader's answers go out
    s_await_until = now_ms() + AWAIT_BOOTLOADER_MS;
    report(OTA_LPC, 0);
    ESP_LOGI(TAG, "LPC reset (user command) to install %s", LPCFW_IMAGE);
    return true;
}

static void do_upgrade(void)
{
    const char *why = atomic_load(&s_session) || atomic_load(&s_await)
                          ? "Error: a motion controller update is already running.\r\n"
                          : not_ready();
    uint32_t size = 0;
    char ver[16], text[200];
    uint8_t *ram = NULL;
    if (!why) {
        why = extract(&size, ver, sizeof(ver));
    }
    if (!why) {
        const char *bad = image_load(&ram, &size);
        if (bad) {
            ESP_LOGE(TAG, "%s: %s", LPCFW_IMAGE, bad);
            why = "Error: cannot load the motion controller firmware.\r\n";
        }
    }
    if (why) {
        atomic_store(&s_claim, false);
        notify(why);
        return;
    }
    image_set(ram, size);
    snprintf(text, sizeof(text), "Info: motion controller firmware %s (%lu bytes) taken from the package.\r\n", ver,
             (unsigned long)size);
    notify(text);
    why = not_ready();
    if (!why && reset_lpc_on_command()) {
        notify("Info: installing it (about a minute).\r\n");
    } else {
        notify(why ? why : "Error: the motion controller could not be reset.\r\n");
        notify("Info: it is installed when the motion controller asks for it at its next start (switch the "
               "machine off and on), or with `upgrade` again; `upgrade cancel` removes it.\r\n");
    }
    atomic_store(&s_claim, false);  // s_await holds it from here
}

static void do_cancel(void)
{
    if (atomic_load(&s_session)) {
        notify("Error: the motion controller is receiving its firmware right now; not cancelled.\r\n");
    } else {
        if (atomic_load(&s_await)) {
            atomic_store(&s_await, false);
            s_await_until = 0;
            lpc_link_console_block(false);
        }
        bool gone = image_drop();
        report(OTA_IDLE, 0);
        s_show_until = 0;
        if (gone) {
            notify("Info: motion controller update cancelled; " LPCFW_IMAGE " removed.\r\n");
        }
    }
    atomic_store(&s_claim, false);
}

static void tick(void)
{
    int64_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    handle_out(lpcfw_sm_tick(&s_sm, now), false);
    bool active = lpcfw_sm_active(&s_sm);
    xSemaphoreGive(s_lock);
    if (s_await_until && now > s_await_until && !active) {
        s_await_until = 0;
        atomic_store(&s_await, false);
        lpc_link_console_block(false);
        report(OTA_ERROR, 0);
        s_show_until = now + SHOW_FAILED_MS;
        notify("Error: the motion controller did not restart into its bootloader. " LPCFW_IMAGE
               " is served when it asks at its next start; `upgrade cancel` removes it.\r\n");
    }
    if (s_show_until && now > s_show_until && !lpcfw_busy()) {
        s_show_until = 0;
        report(OTA_IDLE, 0);
    }
}

static void lpcfw_task(void *arg)
{
    for (;;) {
        msg_t m;
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (m.kind == MSG_UPGRADE) {
                do_upgrade();
            } else if (m.kind == MSG_CANCEL) {
                do_cancel();
            } else {
                on_event(&m);
            }
        }
        tick();
    }
}

esp_err_t lpcfw_start(void)
{
    if (s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(16, sizeof(msg_t));
    if (!s_lock || !s_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (!storage_read_only() && storage_hold()) {
        remove(IMAGE_TMP);  // left by a power cut while a bundle was checked
        storage_release();
    }
    // A waiting image is only ever served when the bootloader asks by itself
    // (an LPC start the user caused, such as a power cycle): never a reset here.
    uint8_t *ram;
    uint32_t size = 0;
    const char *bad = image_load(&ram, &size);
    if (!bad) {
        image_set(ram, size);
        ESP_LOGW(TAG, "%s is waiting: served if the LPC's bootloader asks for it", LPCFW_IMAGE);
    } else if (strcmp(bad, "absent") != 0) {
        ESP_LOGW(TAG, "%s is not served: %s", LPCFW_IMAGE, bad);
    }
    if (xTaskCreate(lpcfw_task, "lpcfw", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void lpcfw_on_lpc_frame(uint8_t type, const uint8_t *payload, size_t n)
{
    bool firm = type >= PTYPE_FIRM_START && type <= PTYPE_FIRM_CAN;
    if (!s_lock || (!firm && !atomic_load(&s_session))) {
        return;  // the LPC firmware talking, with no transfer to end
    }
    if (firm) {
        char h[20];
        hex(h, sizeof(h), payload, n);
        ESP_LOGD(TAG, "FIRM rx %02X len %u [%s]", type, (unsigned)n, h);
    }
    // Answered right here, from RAM: the bootloader's patience is unknown.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rx_us = esp_timer_get_time();
    uint32_t size = s_ram ? s_image : 0;
    if (!lpcfw_sm_active(&s_sm) && ota_active()) {
        size = 0;  // our own restart would cut the transfer: the LPC firmware starts instead
    }
    handle_out(lpcfw_sm_frame(&s_sm, type, payload, n, size, now_ms()), !firm);
    xSemaphoreGive(s_lock);
}

static const char *request(uint8_t kind)
{
    if (!s_queue) {
        return "Error: firmware update not available\r\n";
    }
    if (atomic_exchange(&s_claim, true)) {
        return "Error: a motion controller update is already running.\r\n";
    }
    msg_t m = {.kind = kind};
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        atomic_store(&s_claim, false);
        return "Error: busy, try again\r\n";
    }
    return NULL;
}

const char *lpcfw_upgrade(void)
{
    if (!s_queue) {
        return "Error: firmware update not available\r\n";
    }
    if (atomic_load(&s_session) || atomic_load(&s_await)) {
        return "Error: a motion controller update is already running.\r\n";
    }
    const char *why = not_ready();  // checked again on the task, right before the reset
    return why ? why : request(MSG_UPGRADE);
}

const char *lpcfw_cancel(void)
{
    return request(MSG_CANCEL);
}

const char *lpcfw_reset_warning(void)
{
    return RESET_WARNING;
}

bool lpcfw_status_stand_in(char *out, size_t cap)
{
    unsigned phase, pct;
    lpcfw_ota_report(&phase, &pct);
    char st[MACHINE_STATE_MAX];
    if (phase == OTA_IDLE || machine_get_state(st, sizeof(st))) {
        return false;
    }
    static char last[MACHINE_REPORT_MAX];  // the housekeeping task's only
    const char *rest = machine_get_last_report(last, sizeof(last)) ? strchr(last, '|') : NULL;
    // The LPC's last positions with the state it has before and after its
    // bootloader (it was reset while Idle, or restarted on its own: no job).
    int n = snprintf(out, cap, "<Idle%s\n",  // with a newline, as the LPC sends it
                     rest ? rest : "|MPos:0.0000,0.0000,0.0000,0.0000,0.0000|WPos:0.0000,0.0000,0.0000,0.0000,0.0000>");
    return n > 0 && (size_t)n < cap;
}
