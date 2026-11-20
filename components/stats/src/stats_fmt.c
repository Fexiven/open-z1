#define _POSIX_C_SOURCE 200809L  // gmtime_r

#include "stats_fmt.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int stats_fmt_sn(char *out, size_t cap, const char *sn)
{
    int n = sn ? snprintf(out, cap, "Success:sn = %s\n", sn) : snprintf(out, cap, "sn = null\n");
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

int stats_fmt_sys_time(char *out, size_t cap, bool has_first, int64_t first_boot, uint64_t pon_s, uint64_t mach_s)
{
    char first[24];
    time_t t = (time_t)first_boot;
    struct tm tm;
    if (!has_first || !gmtime_r(&t, &tm) || !strftime(first, sizeof(first), "%Y/%m/%d %H:%M", &tm)) {
        strcpy(first, "null");
    }
    int n = snprintf(out, cap, "sys-time-data = %s,%llu,%llu\n", first, (unsigned long long)pon_s,
                     (unsigned long long)mach_s);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

bool stats_parse_time(const char *s, size_t n, int64_t *out)
{
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) {
        n--;
    }
    if (n < 5 || memcmp(s, "time ", 5) != 0) {
        return false;
    }
    size_t i = 5;
    if (i + 2 <= n && s[i] == '=' && s[i + 1] == ' ') {
        i += 2;
    }
    if (i == n || n - i > 12) {
        return false;
    }
    int64_t v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (s[i] - '0');
    }
    if (v < STATS_CLOCK_MIN) {
        return false;
    }
    *out = v;
    return true;
}
