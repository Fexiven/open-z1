#pragma once
// Liveness of the tasks the machine depends on: the LPC link, job playback and
// the app server. Each registers once and beats at least every second.
// ota_healthy() asks health_recent() before it keeps a new image. Only the LPC
// link is on the task watchdog (CONFIG_ESP_TASK_WDT_PANIC: stuck, it restarts
// the board): it does no SD I/O, whereas playback and the server wait on the
// card, and a slow or failing card must never restart the board under a job.

#include <stdbool.h>
#include <stdint.h>

typedef enum { HEALTH_LPC, HEALTH_PLAY, HEALTH_CTRL, HEALTH_COUNT } health_task_t;

// Call from the task itself, once, before its loop; watchdog: also put it on
// the task watchdog.
void health_watch(health_task_t t, bool watchdog);

// Call from the task in its loop.
void health_beat(health_task_t t);

// Every task that registered beat within max_ms, and all three registered.
bool health_recent(uint32_t max_ms);

// CPU load per core: call once a second (the housekeeping tick); then
// health_cpu_load(core) is 100 minus the idle task's share of the last
// interval, in percent, or -1 before two ticks.
void health_cpu_tick(void);
int health_cpu_load(int core);
