#pragma once
// Pure part of the machine statistics: the stock reply formats of `sn-get`
// and `sys-time`, and clock parsing. Host-tested.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Clock values before this (2020-01-01) are not a real clock (e.g. the LPC
// counting from 0 since boot) and are ignored.
#define STATS_CLOCK_MIN 1577836800LL

// "Success:sn = <sn>\n", or "sn = null\n" when sn is NULL. Returns the
// length, or -1 if it does not fit.
int stats_fmt_sn(char *out, size_t cap, const char *sn);

// "sys-time-data = <first>,<pon_s>,<mach_s>\n", where <first> is
// "YYYY/MM/DD hh:mm" of first_boot (already local time, so no time zone is
// applied), or "null" when has_first is false. Returns the length, or -1.
int stats_fmt_sys_time(char *out, size_t cap, bool has_first, int64_t first_boot, uint64_t pon_s, uint64_t mach_s);

// Parses the app's "time <N>" line or the LPC's "time = <N>" reply (trailing
// CR/LF allowed). Returns true and sets *out for a plausible clock.
bool stats_parse_time(const char *s, size_t n, int64_t *out);
