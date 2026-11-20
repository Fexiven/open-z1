#include "health.h"

#include <stdatomic.h>

#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static _Atomic int64_t s_beat_us[HEALTH_COUNT];  // 0: not registered
static bool s_wdt[HEALTH_COUNT];                 // set once by the task itself

void health_watch(health_task_t t, bool watchdog)
{
    s_wdt[t] = watchdog && esp_task_wdt_add(NULL) == ESP_OK;
    health_beat(t);
}

void health_beat(health_task_t t)
{
    if (s_wdt[t]) {
        esp_task_wdt_reset();
    }
    atomic_store(&s_beat_us[t], esp_timer_get_time());
}

bool health_recent(uint32_t max_ms)
{
    int64_t now = esp_timer_get_time();
    for (int t = 0; t < HEALTH_COUNT; t++) {
        int64_t b = atomic_load(&s_beat_us[t]);
        if (!b || now - b > (int64_t)max_ms * 1000) {
            return false;
        }
    }
    return true;
}

// CPU load from FreeRTOS's run-time stats (esp_timer clock, microseconds,
// 32 bits: the differences stay right across the wrap). An idle task's
// counter grows only when it is switched out, so a core idle without a
// switch for long reads busy until its next switch.
static uint32_t s_idle_prev[CONFIG_FREERTOS_NUMBER_OF_CORES], s_clock_prev;
static _Atomic int s_load[CONFIG_FREERTOS_NUMBER_OF_CORES] = {[0 ... CONFIG_FREERTOS_NUMBER_OF_CORES - 1] = -1};

void health_cpu_tick(void)
{
    uint32_t clock = portGET_RUN_TIME_COUNTER_VALUE(), span = clock - s_clock_prev;
    for (int c = 0; c < CONFIG_FREERTOS_NUMBER_OF_CORES; c++) {
        uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(c);
        if (s_clock_prev && span) {
            uint32_t share = (uint32_t)((uint64_t)(idle - s_idle_prev[c]) * 100 / span);
            atomic_store(&s_load[c], share > 100 ? 0 : 100 - (int)share);
        }
        s_idle_prev[c] = idle;
    }
    s_clock_prev = clock | 1;  // never 0 after the first tick
}

int health_cpu_load(int core)
{
    return core >= 0 && core < CONFIG_FREERTOS_NUMBER_OF_CORES ? atomic_load(&s_load[core]) : -1;
}
