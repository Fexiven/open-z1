#include "sdlog.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdlog_ring.h"
#include "storage.h"

static const char *TAG = "sdlog";

#define FILE_MAX (3u * 1024 * 1024)  // stock: 3 MiB
#define RING_SIZE (32 * 1024)        // in PSRAM; about 300 lines
#define SDLOG_LINE_MAX 600           // an LPC frame (512) with its prefix
#define FLUSH_MS 2000                // stock's flush period
#define RETRY_MS 10000               // after a failed write
#define COPY_MAX 256                 // bytes per critical section
#define CLOCK_SET_S 1600000000       // the app's `time` has set the clock

static vprintf_like_t s_console;
static sdlog_ring_t s_ring;  // under s_mux
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_line_lock;  // s_line and the state after it
static char s_line[SDLOG_LINE_MAX];
static bool s_mid_line, s_mid_skip, s_clock_set;  // a piece without its '\n' came last
static atomic_uint s_dropped;
static __thread bool t_console_only;

void sdlog_console_only(bool on)
{
    t_console_only = on;
}

static void ring_put(const char *s, size_t n)
{
    taskENTER_CRITICAL(&s_mux);
    sdlog_ring_put(&s_ring, s, n);
    taskEXIT_CRITICAL(&s_mux);
}

// Up to cap bytes from *pos on, COPY_MAX per critical section.
static size_t take(uint64_t *pos, char *out, size_t cap, bool *lost)
{
    size_t n = 0;
    while (n < cap) {
        taskENTER_CRITICAL(&s_mux);
        size_t got = sdlog_ring_read(&s_ring, pos, out + n, cap - n < COPY_MAX ? cap - n : COPY_MAX, lost);
        taskEXIT_CRITICAL(&s_mux);
        if (got == 0) {
            break;
        }
        n += got;
    }
    return n;
}

static uint64_t ring_head(void)
{
    taskENTER_CRITICAL(&s_mux);
    uint64_t h = s_ring.head;
    taskEXIT_CRITICAL(&s_mux);
    return h;
}

// "[YYYY-MM-DD hh:mm:ss.mmm] " once the app has set the clock, "[uptime N ms] " before.
static size_t stamp(char *out, size_t cap, bool *clock_set)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *clock_set = tv.tv_sec >= CLOCK_SET_S;
    if (!*clock_set) {
        return (size_t)snprintf(out, cap, "[uptime %lld ms] ", (long long)(esp_timer_get_time() / 1000));
    }
    struct tm tm;
    time_t t = tv.tv_sec;
    gmtime_r(&t, &tm);  // the app sends local time; shown as it came (stock)
    size_t n = strftime(out, cap, "[%Y-%m-%d %H:%M:%S", &tm);
    return n + (size_t)snprintf(out + n, cap - n, ".%03ld] ", (long)(tv.tv_usec / 1000));
}

static void marker(const char *what)
{
    char ts[48], line[96];
    bool clock_set;
    size_t n = stamp(ts, sizeof(ts), &clock_set);
    ts[n - 2] = '\0';  // drop "] "
    ring_put(line, (size_t)snprintf(line, sizeof(line), "===== %s %s] =====\n", what, ts));
}

static void capture(const char *fmt, va_list ap)
{
    if (xSemaphoreTake(s_line_lock, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1);  // never wait: the caller may be anyone
        return;
    }
    size_t n = 0;
    if (!s_mid_line) {
        bool clock_set;
        n = stamp(s_line, sizeof(s_line), &clock_set);
        if (clock_set && !s_clock_set) {
            s_clock_set = true;
            marker("system time synced");
        }
    }
    int m = vsnprintf(s_line + n, sizeof(s_line) - n, fmt, ap);
    size_t body = m < 0 ? 0 : (size_t)m < sizeof(s_line) - n ? (size_t)m : sizeof(s_line) - n - 1;
    bool skip = s_mid_line ? s_mid_skip : sdlog_line_from_wifi_driver(s_line + n, body);
    n += body;
    if (m > 0 && (size_t)m > body) {
        s_line[n - 1] = '\n';  // cut
    }
    if (!skip && body && !(body == 1 && !s_mid_line && s_line[n - 1] == '\n')) {  // not a bare '\n'
        ring_put(s_line, n);
    }
    s_mid_line = n && s_line[n - 1] != '\n';
    s_mid_skip = skip;
    xSemaphoreGive(s_line_lock);
}

static int log_hook(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int r = s_console(fmt, ap);
    if (!t_console_only && !xPortInIsrContext()) {
        capture(fmt, copy);
    }
    va_end(copy);
    return r;
}

size_t sdlog_tail(char *out, size_t cap, unsigned lines, uint64_t *next)
{
    if (cap == 0) {
        return 0;
    }
    size_t n = 0;
    uint64_t pos = 0;
    if (s_ring.buf) {
        uint64_t head = ring_head();
        pos = head > cap - 1 ? head - (cap - 1) : 0;
        bool lost = false, cut = pos > 0;
        n = take(&pos, out, cap - 1, &lost);
        size_t whole = sdlog_whole_lines(out, n);
        pos -= n - whole;  // a line still being written comes with the next read
        n = sdlog_last_lines(out, whole, lines, cut && !lost);  // after a loss it starts at a line
    }
    out[n] = '\0';
    if (next) {
        *next = pos;
    }
    return n;
}

size_t sdlog_since(uint64_t *pos, char *out, size_t cap, bool *gap)
{
    *gap = false;
    if (cap == 0) {
        return 0;
    }
    size_t n = 0;
    if (s_ring.buf) {
        uint64_t head = ring_head(), p = *pos;
        if (p > head || head - p > cap - 1) {  // a cursor from before a restart, or more than fits
            *gap = true;
            p = head > cap - 1 ? head - (cap - 1) : 0;
        }
        bool lost = false, cut = *gap && p > 0;
        n = take(&p, out, cap - 1, &lost);
        size_t start = 0;
        if (cut && !lost) {  // the first line may be partial
            while (start < n && out[start++] != '\n') {
            }
        }
        size_t whole = sdlog_whole_lines(out + start, n - start);
        p -= n - start - whole;
        memmove(out, out + start, whole);
        n = whole;
        *gap |= lost;
        *pos = p;
    }
    out[n] = '\0';
    return n;
}

// .log.1 -> .log.2, .log -> .log.1 (stock: 3 files).
static void rotate(void)
{
    remove(SDLOG_FILE ".2");
    rename(SDLOG_FILE ".1", SDLOG_FILE ".2");
    rename(SDLOG_FILE, SDLOG_FILE ".1");
}

// Appends what the ring holds after *pos. Under a storage hold. As stock, the
// file rotates before a write would take it past FILE_MAX; here at a line end.
static bool append(uint64_t *pos, bool *lost)
{
    static char chunk[4096];
    mkdir(SDLOG_DIR, 0775);  // fails harmlessly when it exists
    struct stat st;
    size_t size = stat(SDLOG_FILE, &st) == 0 ? (size_t)st.st_size : 0;
    FILE *f = NULL;
    bool ok = true;
    size_t n;
    while (ok && (n = take(pos, chunk, sizeof(chunk), lost)) > 0) {
        for (size_t off = 0, len; ok && off < n; off += len) {
            len = n - off;
            if (size + len > FILE_MAX) {
                size_t cut = size < FILE_MAX ? FILE_MAX - size : 0;
                while (cut > 0 && chunk[off + cut - 1] != '\n') {
                    cut--;
                }
                if (cut == 0 && size > 0) {
                    if (f) {
                        fclose(f);
                        f = NULL;
                    }
                    rotate();
                    size = 0;
                    len = 0;
                    continue;
                }
                if (cut > 0) {
                    len = cut;  // up to the last line that fits (cut 0 at size 0: one huge line)
                }
            }
            if ((!f && !(f = fopen(SDLOG_FILE, "a"))) || fwrite(chunk + off, 1, len, f) != len) {
                ok = false;
            }
            size += len;
        }
    }
    if (f && fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

static void flush_task(void *arg)
{
    uint64_t pos = 0;
    bool failing = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(failing ? RETRY_MS : FLUSH_MS));
        bool lost = false;
        unsigned dropped = atomic_exchange(&s_dropped, 0);
        if (dropped) {
            char note[64];
            ring_put(note, (size_t)snprintf(note, sizeof(note), "(%u log lines dropped here)\n", dropped));
        }
        if (pos == ring_head()) {
            continue;  // nothing new: leave the card alone
        }
        if (storage_read_only() || !storage_hold()) {
            pos = ring_head();  // no card to write: the lines stay in RAM only
            continue;
        }
        bool ok = append(&pos, &lost);
        storage_release();
        if (!ok) {
            pos = ring_head();  // don't retry these lines
        }
        if (ok == failing) {
            failing = !ok;
            if (failing) {
                ESP_LOGW(TAG, "cannot write %s; retrying every %d s", SDLOG_FILE, RETRY_MS / 1000);
            } else {
                ESP_LOGI(TAG, "writing %s again", SDLOG_FILE);
            }
        }
        if (lost) {
            static const char k_lost[] = "(log lines lost here: the card was too slow)\n";
            ring_put(k_lost, sizeof(k_lost) - 1);
        }
    }
}

esp_err_t sdlog_start(void)
{
    char *buf = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
    size_t size = RING_SIZE;
    if (!buf) {
        size = RING_SIZE / 8;
        buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    s_line_lock = xSemaphoreCreateMutex();
    if (!buf || !s_line_lock) {
        return ESP_ERR_NO_MEM;
    }
    sdlog_ring_init(&s_ring, buf, size);
    char ts[48];
    stamp(ts, sizeof(ts), &s_clock_set);  // kept over a restart: no "synced" marker then
    marker("log session start");
    if (xTaskCreate(flush_task, "sdlog", 6144, NULL, 1, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_console = esp_log_set_vprintf(log_hook);
    return ESP_OK;
}
