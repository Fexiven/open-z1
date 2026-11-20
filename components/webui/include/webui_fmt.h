#pragma once
// The web UI's status JSON and log redaction. Pure, host-tested.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Strings are never NULL ("" when unknown).
typedef struct {
    bool fresh;                       // a status report from the LPC within 2 s
    const char *state;                // its state word ("Idle", "Run", "Hold:0", ...)
    const char *report;               // the LPC's status report, "" unless fresh
    bool lpc_silent;                  // stock's alarm: no frame from the LPC for 10 s
    uint32_t lpc_rate_x10;            // status reports per second, times 10
    const char *lpc_version, *lpc_config;
    const char *job_file;             // the job being served, or the last one since boot
    bool job_serving;
    bool sd_mounted;
    uint32_t sd_used_mb, sd_total_mb;
    bool wifi_connected;
    int rssi;
    const char *ip;
    uint32_t uptime_s;
    const char *reset_reason;
    const char *version, *slot, *slot_state, *other_slot, *other_version;
    uint32_t heap_free, heap_min, psram_free, psram_min;
    int cpu[2];                       // load per core in percent, -1: not yet known
} webui_status_t;

// Writes s as one JSON object. Returns its length, or 0 if it does not fit.
// From the report: machine.mpos, .wpos and job.progress ("lines,pct,secs") as
// the LPC sends them, and its F, S, T, H, A, L and C fields, where present,
// as arrays of numbers (null for anything else, such as nan); Kernel.cpp
// get_query_string has their meaning.
size_t webui_status_json(const webui_status_t *s, char *out, size_t cap);

// Replaces every dotted IPv4 address in text other than keep (the board's
// own) with "x.x.x.x", in place: the log names the app's clients.
void webui_redact_ips(char *text, const char *keep);

// A decimal number of at most 20 digits that fits in 64 bits, nothing else
// (the log cursor, ?since=). Returns whether s is one.
bool webui_parse_u64(const char *s, uint64_t *v);
