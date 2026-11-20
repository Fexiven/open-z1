#pragma once
// Machine statistics as stock keeps them: the factory serial number and, in
// NVS namespace "runtime", the first-boot date and the power-on and machine
// seconds. Answers the app's `sn-get` and `sys-time`.
//
// The system clock is set from these readings, so it runs in LOCAL time (the
// app sends local time), not UTC; mind that for certificate checks (TLS, OTA).

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Loads the counters; stats_tick then counts and commits the power-on and
// machine seconds to NVS every 10 minutes. job_running: whether a job runs right now (counted
// into the machine seconds). Call after nvs_flash_init.
esp_err_t stats_start(bool (*job_running)(void));

// Counts a second (machine seconds while a job runs) and commits the counters
// every 10 minutes. Main's housekeeping calls it once a second.
void stats_tick(void);

// Feed console lines that may carry the clock: the app's `time <N>` and the
// LPC's `time = <N>` reply. Anything else is ignored. Sets the system clock
// and, the first time ever, records the first boot. Safe from any task.
void stats_on_line(const char *line, size_t n);

// The replies to `sn-get` and `sys-time`. Return the length, or -1.
int stats_sn_reply(char *out, size_t cap);
int stats_sys_time_reply(char *out, size_t cap);
