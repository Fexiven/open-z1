#pragma once
// Latest machine state reported by the LPC, for the LAN announcement and job
// playback. machine_status_state is pure and host-tested (machine_status.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The LPC is polled every 500 ms; without a status for this long the state is
// considered unknown.
#define MACHINE_STATE_STALE_MS 2000

// Buffer size that holds any state word, terminator included.
#define MACHINE_STATE_MAX 16

// Buffer size that holds any status report (the LPC sends at most 512 bytes).
#define MACHINE_REPORT_MAX 513

// Feed every STATUS_RES payload from the LPC.
void machine_on_status(const char *payload, size_t n);

// Status reports per second over the last window of about a second, times
// 10 (the keepalive alone gives 20); 0 once none came for 3 s.
uint32_t machine_status_rate_x10(void);

// Copies the last reported state word and returns true if it is fresh
// (reported within MACHINE_STATE_STALE_MS). Returns false, leaving out
// untouched, if the LPC never reported or has gone silent.
bool machine_get_state(char *out, size_t cap);

// Copies the last status report as the LPC sent it, without a trailing
// newline, and returns true if it is fresh (as machine_get_state). Otherwise
// returns false with out empty. cap must be at least MACHINE_REPORT_MAX.
bool machine_get_report(char *out, size_t cap);

// As machine_get_report, but also a stale report: false only if the LPC
// never reported since boot.
bool machine_get_last_report(char *out, size_t cap);

// Copies the value of the field `name` ("MPos", "WPos", "P", ...) of the
// NUL-terminated status report s, e.g. "-1.0000,-1.0000,-3.0000,0.0000,0.0000".
// False if the field is absent or its value does not fit.
bool machine_status_field(const char *s, const char *name, char *out, size_t cap);

// Extracts the state word of the LPC's Grbl-style status report
// ("<Idle|MPos:...|...>"): "Idle", "Run", "Hold:0", "Alarm", ..., ended by
// '|', '>' or (older report format) ','. Accepts only letters, digits and ':'
// so the result is safe to put in a CSV field. Returns false if s is not a
// status report or the state does not fit.
bool machine_status_state(const char *s, size_t n, char *out, size_t cap);
