#include "machine.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"


static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_state[MACHINE_STATE_MAX];
static char s_report[MACHINE_REPORT_MAX];
static int64_t s_updated_us;  // 0: never
static int64_t s_win_us;      // start of the current rate window
static uint32_t s_win_count, s_rate_x10;

void machine_on_status(const char *payload, size_t n)
{
    char st[MACHINE_STATE_MAX];
    if (!machine_status_state(payload, n, st, sizeof(st))) {
        return;
    }
    while (n > 0 && (payload[n - 1] == '\n' || payload[n - 1] == '\r')) {
        n--;
    }
    n = n < sizeof(s_report) - 1 ? n : sizeof(s_report) - 1;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_state, st, sizeof(s_state));
    memcpy(s_report, payload, n);
    s_report[n] = '\0';
    s_updated_us = now;
    if (now - s_win_us >= 1000000) {  // a window of at least 1 s ends with this report
        s_rate_x10 = now - s_win_us < 3000000 ? (uint32_t)(s_win_count * 10000000LL / (now - s_win_us)) : 0;
        s_win_us = now;
        s_win_count = 0;
    }
    s_win_count++;
    taskEXIT_CRITICAL(&s_mux);
}

uint32_t machine_status_rate_x10(void)
{
    taskENTER_CRITICAL(&s_mux);
    int64_t win = s_win_us;
    uint32_t rate = s_rate_x10;
    taskEXIT_CRITICAL(&s_mux);
    return esp_timer_get_time() - win > 3000000 ? 0 : rate;
}

bool machine_get_report(char *out, size_t cap)
{
    if (cap < MACHINE_REPORT_MAX) {
        return false;
    }
    taskENTER_CRITICAL(&s_mux);
    int64_t updated = s_updated_us;
    memcpy(out, s_report, MACHINE_REPORT_MAX);
    taskEXIT_CRITICAL(&s_mux);
    if (updated == 0 || esp_timer_get_time() - updated > (int64_t)MACHINE_STATE_STALE_MS * 1000) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool machine_get_last_report(char *out, size_t cap)
{
    if (cap < MACHINE_REPORT_MAX) {
        return false;
    }
    taskENTER_CRITICAL(&s_mux);
    int64_t updated = s_updated_us;
    memcpy(out, s_report, MACHINE_REPORT_MAX);
    taskEXIT_CRITICAL(&s_mux);
    return updated != 0;
}

bool machine_get_state(char *out, size_t cap)
{
    char st[MACHINE_STATE_MAX];
    taskENTER_CRITICAL(&s_mux);
    int64_t updated = s_updated_us;
    memcpy(st, s_state, sizeof(st));
    taskEXIT_CRITICAL(&s_mux);

    if (updated == 0 || esp_timer_get_time() - updated > (int64_t)MACHINE_STATE_STALE_MS * 1000) {
        return false;
    }
    size_t len = strnlen(st, sizeof(st) - 1);
    if (cap == 0 || len >= cap) {
        return false;
    }
    memcpy(out, st, len);
    out[len] = '\0';
    return true;
}
