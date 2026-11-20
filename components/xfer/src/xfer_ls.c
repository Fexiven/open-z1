#define _POSIX_C_SOURCE 200809L  // gmtime_r

#include "xfer_ls.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

bool xfer_ls_listed(const char *name)
{
    return name[0] && strcmp(name, ".") && strcmp(name, "..");  // stock lists .md5/ and .lz/ too
}

int xfer_ls_line(char *out, size_t cap, const char *name, bool dir, uint32_t size, int64_t mtime, bool sizes)
{
    char esc[256];
    size_t n = strlen(name);
    if (n >= sizeof(esc)) {
        return -1;
    }
    for (size_t i = 0; i <= n; i++) {
        esc[i] = name[i] == ' ' ? 0x01 : name[i];
    }
    int len;
    if (sizes) {
        time_t t = (time_t)mtime;
        struct tm tm;
        if (!gmtime_r(&t, &tm)) {
            memset(&tm, 0, sizeof(tm));
            tm.tm_year = 80;  // FAT's earliest date
            tm.tm_mday = 1;
        }
        // After the name a '/' for a folder or a space for a file: "dir/ 0 ..",
        // "file  size .." (as stock).
        len = snprintf(out, cap, "%s%c %u %04d%02d%02d%02d%02d%02d\r\n", esc, dir ? '/' : ' ', dir ? 0u : (unsigned)size,
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        len = snprintf(out, cap, "%s%s\r\n", esc, dir ? "/" : "");
    }
    return len < 0 || (size_t)len >= cap ? -1 : len;
}
