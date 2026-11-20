#include "cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cfg_text.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lpc_link.h"
#include "mkframe.h"
#include "storage.h"
#include "xfer_fs.h"

static const char *TAG = "cfg";

#define CONFIG_TXT "/sd/config.txt"
#define CONFIG_DEFAULT "/sd/config.default"

#define FILE_MAX (64 * 1024)  // config.txt is about 6 KB
#define LPC_CHUNK 480         // data bytes per CONFIG_DATA (stock sends about 440)

// Reads a whole file into a malloc'd buffer (caller frees). NULL if missing.
static char *load(const char *path, size_t *n)
{
    if (!storage_hold()) {
        return NULL;
    }
    if (!storage_read_only()) {
        xfer_fs_recover(path);  // a replacement cut short by a power loss
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        storage_release();
        return NULL;
    }
    struct stat st;
    size_t size = fstat(fileno(f), &st) == 0 && st.st_size >= 0 ? (size_t)st.st_size : FILE_MAX;
    char *buf = size < FILE_MAX ? malloc(size + 1) : NULL;  // +1: room to see the file grew
    *n = buf ? fread(buf, 1, size + 1, f) : 0;
    bool ok = buf && !ferror(f) && *n <= size;
    if (ok) {
        buf[*n] = '\0';
    }
    fclose(f);
    storage_release();
    if (!ok) {
        free(buf);
        return NULL;
    }
    return buf;
}

// Replaces path (config.txt or config.default) with data, atomically
// (xfer_fs_keep). No .md5 sidecar: downloads hash the content, so a copy
// edited on a PC is never stale.
static bool store(const char *path, const char *data, size_t n)
{
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "/sd/.%s.part", path + 4);
    if (!storage_hold()) {
        return false;
    }
    FILE *f = fopen(tmp, "wb");
    bool ok = f != NULL;
    if (f) {
        ok = fwrite(data, 1, n, f) == n;
        ok &= fclose(f) == 0;
        ok = ok && xfer_fs_keep(tmp, path, NULL);
        if (!ok) {
            remove(tmp);
        }
    }
    storage_release();
    return ok;
}

static void config_get(const char *a1, const char *a2, char *out, size_t cap)
{
    // INFERRED: with no source, stock answers "cached:" from the file too.
    bool cached = !a2;
    const char *source = cached ? "cached" : a1, *key = cached ? a1 : a2;
    if (!key) {
        snprintf(out, cap, "Usage: config-get [<configuration_source>] <configuration_setting>\r\n");
        return;
    }
    if (!cached && strcmp(source, "sd")) {
        snprintf(out, cap, "%s source does not exist\r\n", source);
        return;
    }
    size_t n;
    char *text = load(CONFIG_TXT, &n);
    char value[64];
    if (text && cfg_get(text, n, key, value, sizeof(value))) {
        snprintf(out, cap, "%s: %s is set to %s\r\n", source, key, value);
    } else {
        snprintf(out, cap, "%s: %s is not in config\r\n", source, key);
    }
    free(text);
}

static void config_set(const char *source, const char *key, const char *value, char *out, size_t cap)
{
    if (!source || !key || !value) {
        snprintf(out, cap, "Usage: config-set source setting value # where source is sd, setting is the key and "
                           "value is the new value\r\n");
        return;
    }
    if (strcmp(source, "sd")) {
        snprintf(out, cap, "%s source does not exist\r\n", source);
        return;
    }
    if (!cfg_key_ok(key) || !cfg_value_ok(value)) {
        snprintf(out, cap, "Error: invalid setting or value\r\n");  // INFERRED wording
        return;
    }
    size_t n;
    char *text = load(CONFIG_TXT, &n);
    char *next = text ? malloc(n + 256) : NULL;
    long len = next ? cfg_set(text, n, key, value, next, n + 256) : -1;
    // A new line the LPC would not get (its limit is CFG_LINE_MAX): refused.
    uint32_t too_long = len >= 0 ? cfg_lpc_long_line(next, (size_t)len) : 0;
    if (too_long && too_long != cfg_lpc_long_line(text, n)) {
        free(text);
        free(next);
        snprintf(out, cap, "Error: %s would make line %lu longer than %d characters, which the machine ignores\r\n",
                 key, (unsigned long)too_long, CFG_LINE_MAX);
        return;
    }
    bool ok = len >= 0 && store(CONFIG_TXT, next, (size_t)len);
    free(text);
    free(next);
    if (ok) {
        ESP_LOGD(TAG, "config-set %s %s: saved", key, value);
    } else {
        ESP_LOGW(TAG, "config-set %s: failed", key);
    }
    if (ok) {
        snprintf(out, cap, "sd: %s has been set to %s\r\n", key, value);
    } else {
        snprintf(out, cap, "Config file not found or created fail: %s\r\n", CONFIG_TXT);
    }
}

static void config_restore(char *out, size_t cap)
{
    size_t n;
    char *text = load(CONFIG_DEFAULT, &n);
    if (!text) {
        snprintf(out, cap, "Default file not found: %s\r\n", CONFIG_DEFAULT);
        return;
    }
    bool ok = store(CONFIG_TXT, text, n);
    free(text);
    ESP_LOGI(TAG, "config-restore: %s", ok ? "config.txt replaced by config.default" : "failed");
    snprintf(out, cap, ok ? "Settings restored complete.\r\n" : "Config file not found or created fail: %s\r\n",
             CONFIG_TXT);
}

// config.txt -> config.default, as stock (the only way config.default is written).
static void config_save_default(char *out, size_t cap)
{
    size_t n;
    char *text = load(CONFIG_TXT, &n);
    if (!text) {
        snprintf(out, cap, "Config file not found: %s\r\n", CONFIG_TXT);
        return;
    }
    bool ok = store(CONFIG_DEFAULT, text, n);
    free(text);
    ESP_LOGI(TAG, "config-default: %s", ok ? "config.default replaced by config.txt" : "failed");
    snprintf(out, cap, ok ? "Settings save as default complete.\r\n" : "Default file not found or created fail: %s\r\n",
             CONFIG_DEFAULT);
}

void cfg_command(const char *cmd, const char *a1, const char *a2, const char *a3, char *out, size_t cap)
{
    bool write = strcmp(cmd, "config-get") != 0;
    if (!storage_mounted()) {
        snprintf(out, cap, "Error: no SD card\r\n");
    } else if (write && storage_read_only()) {
        snprintf(out, cap, "Error: the SD card is read-only in this firmware\r\n");
    } else if (!strcmp(cmd, "config-get")) {
        config_get(a1, a2, out, cap);
    } else if (!strcmp(cmd, "config-set")) {
        config_set(a1, a2, a3, out, cap);
    } else if (!strcmp(cmd, "config-restore")) {
        config_restore(out, cap);
    } else {
        config_save_default(out, cap);
    }
}

// ---- the LPC's config load (MakeraZ1Firmware FileConfigSource.cpp)
//
//   LPC CONFIG_START                 us CONFIG_START (ack)
//   LPC CONFIG_VIEW (a request)      us CONFIG_VIEW {u32 lines, u16 512}
//   LPC CONFIG_DATA {u32 line n}     us CONFIG_DATA {u32 n, lines n.. joined by '\n'}
//   LPC CONFIG_END                   done
// Without an answer within about 2 s the LPC runs on its built-in defaults.

// What GET /ota and the console show; written by the LPC link task only.
typedef enum { LOAD_NONE, LOAD_SERVING, LOAD_SERVED, LOAD_CANCELLED, LOAD_NO_FILE } load_state_t;
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static load_state_t s_state;
static uint32_t s_state_lines, s_state_ms;

static void set_state(load_state_t st, uint32_t lines)
{
    uint32_t ms = esp_log_timestamp();  // the same clock as the log lines
    portENTER_CRITICAL(&s_status_mux);
    s_state = st, s_state_lines = lines, s_state_ms = ms;
    portEXIT_CRITICAL(&s_status_mux);
}

void cfg_lpc_status(char *out, size_t cap)
{
    portENTER_CRITICAL(&s_status_mux);
    load_state_t st = s_state;
    uint32_t lines = s_state_lines, ms = s_state_ms;
    portEXIT_CRITICAL(&s_status_mux);
    switch (st) {
    case LOAD_NONE:
        snprintf(out, cap, "not requested since boot");
        break;
    case LOAD_SERVING:
        snprintf(out, cap, "serving %lu lines since %lu ms", (unsigned long)lines, (unsigned long)ms);
        break;
    case LOAD_SERVED:
        snprintf(out, cap, "served %lu lines at %lu ms", (unsigned long)lines, (unsigned long)ms);
        break;
    case LOAD_CANCELLED:
        snprintf(out, cap, "cancelled at %lu ms", (unsigned long)ms);
        break;
    case LOAD_NO_FILE:
        snprintf(out, cap, "failed: no %s at %lu ms", CONFIG_TXT, (unsigned long)ms);
        break;
    }
}

static char *s_text;  // config.txt during a transfer (LPC link task only)
static size_t s_len;
static uint32_t s_lines;

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24), b[1] = (uint8_t)(v >> 16), b[2] = (uint8_t)(v >> 8), b[3] = (uint8_t)v;
}

// cfg_task only: the card is read here, never in the LPC's receive task,
// whose UART buffer covers only about 180 ms.
static void handle_lpc(uint8_t type, const uint8_t *payload, size_t n)
{
    static uint8_t out[4 + LPC_CHUNK];
    switch (type) {
    case PTYPE_CONFIG_START:
        // Reload on every START (the LPC repeats it until it hears us): a copy
        // left by an abandoned load would be stale after a config-set.
        free(s_text);
        s_text = storage_mounted() ? load(CONFIG_TXT, &s_len) : NULL;
        if (!s_text && storage_mounted()) {
            vTaskDelay(pdMS_TO_TICKS(100));  // once more: a busy card is not a missing file
            s_text = load(CONFIG_TXT, &s_len);
        }
        if (!s_text) {
            ESP_LOGW(TAG, "LPC asked for its config, but there is no %s; it runs on its defaults", CONFIG_TXT);
            set_state(LOAD_NO_FILE, 0);
            lpc_link_send(PTYPE_CONFIG_CAN, NULL, 0);
            return;
        }
        s_lines = cfg_lpc_count(s_text, s_len);
        ESP_LOGI(TAG, "LPC loads its config: %lu lines", (unsigned long)s_lines);
        set_state(LOAD_SERVING, s_lines);
        uint32_t too_long = cfg_lpc_long_line(s_text, s_len);
        if (too_long) {
            ESP_LOGW(TAG, "%s line %lu is longer than %d bytes; the LPC does not get it", CONFIG_TXT,
                     (unsigned long)too_long, CFG_LINE_MAX);
        }
        lpc_link_send(PTYPE_CONFIG_START, NULL, 0);
        return;
    case PTYPE_CONFIG_VIEW:
        if (s_text) {
            put32(out, s_lines);
            out[4] = 512 >> 8, out[5] = 512 & 0xFF;  // stock's frame size; the LPC does not use it
            lpc_link_send(PTYPE_CONFIG_VIEW, out, 6);
        }
        return;
    case PTYPE_CONFIG_DATA:
        if (s_text && n >= 4) {
            uint32_t first = (uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16 | payload[2] << 8 | payload[3];
            uint32_t lines;
            size_t len = cfg_lpc_chunk(s_text, s_len, first, (char *)out + 4, LPC_CHUNK, &lines);
            if (!lines) {
                lpc_link_send(PTYPE_CONFIG_END, NULL, 0);
                return;
            }
            put32(out, first);
            lpc_link_send(PTYPE_CONFIG_DATA, out, 4 + len);
        }
        return;
    case PTYPE_CONFIG_END:
    case PTYPE_CONFIG_CAN:
        if (s_text) {
            ESP_LOGI(TAG, "LPC config load %s", type == PTYPE_CONFIG_END ? "complete" : "cancelled");
            set_state(type == PTYPE_CONFIG_END ? LOAD_SERVED : LOAD_CANCELLED, s_lines);
        }
        free(s_text);
        s_text = NULL;
        return;
    default:
        return;
    }
}

typedef struct {
    uint8_t type, n;
    uint8_t data[8];  // the LPC's CONFIG_* frames carry at most a line number
} cfg_msg_t;

static QueueHandle_t s_queue;

static void cfg_task(void *arg)
{
    for (;;) {
        cfg_msg_t m;
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) == pdTRUE) {
            handle_lpc(m.type, m.data, m.n);
        }
    }
}

esp_err_t cfg_start(void)
{
    if (s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    s_queue = xQueueCreate(8, sizeof(cfg_msg_t));
    if (!s_queue || xTaskCreate(cfg_task, "cfg", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void cfg_on_lpc_frame(uint8_t type, const uint8_t *payload, size_t n)
{
    cfg_msg_t m = {.type = type, .n = (uint8_t)(n < sizeof(m.data) ? n : sizeof(m.data))};
    memcpy(m.data, payload, m.n);
    if (!s_queue || xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "config frame 0x%02X dropped (the LPC asks again)", type);
    }
}
