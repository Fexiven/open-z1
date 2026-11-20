#include "stats.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "stats_fmt.h"

static const char *TAG = "stats";

#define NVS_NS "runtime"
#define PON_COMMIT_MS (10 * 60 * 1000)
#define SN_MAX 64

// Set once in stats_start, read-only afterwards.
static uint64_t s_pon_base;  // power-on seconds before this boot
static bool s_pon_writable, s_mach_writable;
static bool (*s_job_running)(void);

// s_lock guards the machine seconds and the first boot.
static SemaphoreHandle_t s_lock;
static uint64_t s_mach_s;  // seconds a job ran, all boots
static bool s_has_first;
static bool s_first_writable;
static bool s_first_warned;
static int64_t s_first_boot;

// A value is only ever written if it was read fine or is truly absent: a stock
// counter we failed to read must not be overwritten.
static bool readable(esp_err_t err)
{
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

// Reads the serial number into sn; false if there is none.
static bool read_sn(char sn[SN_MAX])
{
    nvs_handle_t h;
    if (nvs_open(BOARD_NVS_FACTORY_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = SN_MAX;
    bool ok = nvs_get_str(h, BOARD_NVS_MACHINE_SN, sn, &len) == ESP_OK;
    nvs_close(h);
    return ok;
}

static uint64_t pon_now(void)
{
    return s_pon_base + (uint64_t)(esp_timer_get_time() / 1000000);
}

static void commit(void)
{
    if (!s_pon_writable && !s_mach_writable) {
        return;  // both were unreadable: never overwrite them
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t mach = s_mach_s;
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (s_pon_writable) {
            err = nvs_set_u64(h, "pon_s", pon_now());
        }
        if (err == ESP_OK && s_mach_writable) {
            err = nvs_set_u64(h, "mach_s", mach);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not save the counters: %s", esp_err_to_name(err));
    }
}

void stats_tick(void)
{
    static unsigned t;
    if (!s_lock) {
        return;
    }
    t++;
    if (s_job_running()) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_mach_s++;
        xSemaphoreGive(s_lock);
    }
    if (t % (PON_COMMIT_MS / 1000) == 0) {
        commit();
    }
}

esp_err_t stats_start(bool (*job_running)(void))
{
    if (s_lock || !job_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_job_running = job_running;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        esp_err_t e = nvs_get_i64(h, "first_boot", &s_first_boot);
        s_has_first = e == ESP_OK;
        s_first_writable = readable(e);
        s_pon_writable = readable(nvs_get_u64(h, "pon_s", &s_pon_base));
        s_mach_writable = readable(nvs_get_u64(h, "mach_s", &s_mach_s));
        nvs_close(h);
    } else {
        // NOT_FOUND: no namespace yet, so nothing stored to protect.
        s_first_writable = s_pon_writable = s_mach_writable = err == ESP_ERR_NVS_NOT_FOUND;
    }

    // Never log the serial number itself.
    char sn[SN_MAX];
    bool has_sn = read_sn(sn);
    ESP_LOGI(TAG, "sn %s, first boot %s, power-on %llu s, machine %llu s", has_sn ? "present" : "absent",
             s_has_first ? "known" : "unknown", (unsigned long long)s_pon_base, (unsigned long long)s_mach_s);

    if (!s_pon_writable || !s_mach_writable) {
        ESP_LOGW(TAG, "a stored counter was unreadable; not saving it, to keep the stored value");
    }
    return ESP_OK;
}

static void on_clock(int64_t unix_local)
{
    struct timeval tv = {.tv_sec = (time_t)unix_local};
    settimeofday(&tv, NULL);
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool record = !s_has_first && s_first_writable;
    bool warn = !s_has_first && !s_first_writable && !s_first_warned;
    if (record) {
        s_has_first = true;
        s_first_boot = unix_local;
    }
    s_first_warned |= warn;
    xSemaphoreGive(s_lock);
    if (warn) {
        ESP_LOGW(TAG, "first boot was unreadable; not recording it, to keep the stored value");
    }
    if (!record) {
        return;
    }
    // Set once, the first time the clock is known, and never overwritten.
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i64(h, "first_boot", unix_local);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "first boot recorded");
    } else {
        ESP_LOGW(TAG, "could not save the first boot: %s", esp_err_to_name(err));
    }
}

void stats_on_line(const char *line, size_t n)
{
    int64_t clock;
    if (stats_parse_time(line, n, &clock)) {
        on_clock(clock);
    }
}

int stats_sn_reply(char *out, size_t cap)
{
    char sn[SN_MAX];
    return stats_fmt_sn(out, cap, read_sn(sn) ? sn : NULL);
}

int stats_sys_time_reply(char *out, size_t cap)
{
    if (!s_lock) {
        return stats_fmt_sys_time(out, cap, false, 0, 0, 0);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has = s_has_first;
    int64_t first = s_first_boot;
    uint64_t mach = s_mach_s;
    xSemaphoreGive(s_lock);
    return stats_fmt_sys_time(out, cap, has, first, pon_now(), mach);
}
