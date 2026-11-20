#include "play.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "health.h"
#include "lpc_link.h"
#include "ota.h"
#include "machine.h"
#include "play_sm.h"
#include "storage.h"
#include "xfer_fs.h"
#include "xfer_path.h"

static const char *TAG = "play";

typedef struct {
    uint8_t type;  // 0: arm with path and crc
    uint8_t n;
    uint16_t crc;
    uint8_t data[8];  // the LPC's requests are at most 8 bytes
    char path[XFER_PATH_MAX + 1];
} msg_t;

static QueueHandle_t s_queue;
static volatile bool s_serving;

// Task-owned.
static play_sm_t s_sm;
static FILE *s_file;
static char s_pending[XFER_PATH_MAX + 1], s_last[XFER_PATH_MAX + 1];
static uint8_t s_out[PLAY_OUT_MAX];

// A copy of s_last for other tasks (FILE_PLAYING).
static char s_shown[XFER_PATH_MAX + 1];
static portMUX_TYPE s_shown_mux = portMUX_INITIALIZER_UNLOCKED;

static int file_read(void *ctx, uint32_t offset, uint8_t *buf, size_t n)
{
    FILE *f = ctx;
    if (!f || fseek(f, (long)offset, SEEK_SET) != 0) {
        return -1;
    }
    size_t got = fread(buf, 1, n, f);
    return ferror(f) ? -1 : (int)got;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void close_file(void)
{
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
        storage_release();
    }
}

static void send(const play_out_t *o)
{
    if (o->type) {
        lpc_link_send(o->type, s_out, o->len);
    }
}

#define MSG_LPC_RESET 0xFF  // not a frame type

static void (*s_notify)(const char *text);

void play_set_notify(void (*notify)(const char *text))
{
    s_notify = notify;
}

static void handle(const msg_t *m)
{
    if (m->type == MSG_LPC_RESET) {
        if (play_serving(&s_sm)) {
            ESP_LOGW(TAG, "the LPC restarted during a job; the job is over");
        }
        close_file();
        play_lpc_reset(&s_sm);
        return;
    }
    if (m->type == PTYPE_PLAY_START && ota_active()) {
        // Not while an update may restart the board (the button restart can
        // start a job at any time).
        ESP_LOGW(TAG, "job refused: an update is being installed");
        lpc_link_send(PTYPE_PLAY_CAN, NULL, 0);
        return;
    }
    if (m->type == 0) {
        snprintf(s_pending, sizeof(s_pending), "%s", m->path);
        play_arm(&s_sm, m->crc, now_ms());
        return;
    }
    play_out_t o = play_on_lpc(&s_sm, m->type, m->data, m->n, file_read, s_file, s_out, now_ms());
    if (o.close) {
        close_file();
    }
    if (o.open) {
        const char *path = o.open == PLAY_FILE_PENDING ? s_pending : s_last;
        struct stat st;
        bool held = storage_hold();
        if (held && !storage_read_only()) {
            xfer_fs_recover(path);  // a replacement cut short by a power loss
        }
        s_file = held ? fopen(path, "rb") : NULL;
        if (held && !s_file) {
            storage_release();
        }
        if (s_file) {
            setvbuf(s_file, NULL, _IOFBF, 1024);  // the core reads line by line
        }
        // A size past INT32_MAX would not survive the offsets (FAT allows 4 GB).
        if (s_file && fstat(fileno(s_file), &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
            (uint64_t)st.st_size <= INT32_MAX) {
            ESP_LOGI(TAG, "job %s%s (%ld bytes)", path, o.open == PLAY_FILE_LAST ? " again (restart)" : "",
                     (long)st.st_size);
            if (o.open == PLAY_FILE_PENDING) {
                memcpy(s_last, s_pending, sizeof(s_last));
                portENTER_CRITICAL(&s_shown_mux);
                memcpy(s_shown, s_last, sizeof(s_shown));
                portEXIT_CRITICAL(&s_shown_mux);
            }
            o = play_opened(&s_sm, (uint32_t)st.st_size, s_out);
        } else {
            ESP_LOGW(TAG, "job %s: cannot open", path);
            o = play_open_failed(&s_sm);  // its close shuts a file fopen did open
        }
    }
    send(&o);
    while (o.more) {
        o = play_goto_step(&s_sm, file_read, s_file, s_out);
        send(&o);
    }
    if (o.type == PTYPE_PLAY_END) {
        ESP_LOGI(TAG, "job: end of file sent");
    } else if (o.type == PTYPE_PLAY_CAN && o.too_long) {
        // Stock sends such a line empty, silently; this firmware stops instead.
        char text[160];
        snprintf(text, sizeof(text),
                 "Error: line %lu has more than %d characters of G-code (without comments and spaces); job stopped\r\n",
                 (unsigned long)o.too_long_line + 1, LPC_LINE_MAX);
        ESP_LOGE(TAG, "%.*s", (int)strlen(text) - 2, text);
        if (s_notify) {
            s_notify(text);
        }
    } else if (o.type == PTYPE_PLAY_CAN) {
        ESP_LOGW(TAG, "job cancelled (read error or a request for no armed job)");
    } else if (m->type == PTYPE_PLAY_CAN || m->type == PTYPE_PLAY_END) {
        ESP_LOGI(TAG, "job %s", m->type == PTYPE_PLAY_CAN ? "aborted by the LPC" : "finished");
    }
    if (o.close) {
        close_file();
    }
}

static void play_task(void *arg)
{
    health_watch(HEALTH_PLAY, false);  // waits on the card: no watchdog
    for (;;) {
        msg_t m;
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(500)) == pdTRUE) {
            handle(&m);
            s_serving = play_serving(&s_sm);
        }
        health_beat(HEALTH_PLAY);
    }
}

void play_job_lpc_reset(void)
{
    msg_t m = {.type = MSG_LPC_RESET};
    if (s_queue && xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full; LPC restart not passed on");
    }
}

esp_err_t play_start(void)
{
    if (s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    s_queue = xQueueCreate(8, sizeof(msg_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(play_task, "play", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static _Atomic int64_t s_armed_us = INT64_MIN / 2;  // for play_job_busy

void play_job_arm(const char *path, uint16_t crc)
{
    atomic_store(&s_armed_us, esp_timer_get_time());
    msg_t m = {.crc = crc};
    snprintf(m.path, sizeof(m.path), "%s", path);
    if (s_queue && xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full; play not armed");
    }
}

void play_job_on_lpc_frame(uint8_t type, const uint8_t *payload, size_t n)
{
    msg_t m = {.type = type, .n = (uint8_t)(n < sizeof(m.data) ? n : sizeof(m.data))};
    memcpy(m.data, payload, m.n);
    if (s_queue && xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full; dropped LPC frame 0x%02X (the LPC asks again)", type);
    }
}

void play_job_file(char *out, size_t cap)
{
    char copy[sizeof(s_shown)];
    portENTER_CRITICAL(&s_shown_mux);
    memcpy(copy, s_shown, sizeof(copy));
    portEXIT_CRITICAL(&s_shown_mux);
    snprintf(out, cap, "%s", copy);
}

bool play_job_serving(void)
{
    return s_serving;
}

bool play_job_busy(void)
{
    return s_serving || esp_timer_get_time() - atomic_load(&s_armed_us) < PLAY_ARM_TTL_MS * 1000LL;
}

bool play_job_running(void)
{
    char state[MACHINE_STATE_MAX];
    return s_serving && machine_get_state(state, sizeof(state)) && !strcmp(state, "Run");
}
