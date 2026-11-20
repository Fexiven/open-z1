#include "ota_version.h"

#include <ctype.h>

// Reads the next dot-separated number; returns -1 when there is none.
static long next_part(const char **p)
{
    if (!isdigit((unsigned char)**p)) {
        return -1;
    }
    long v = 0;
    while (isdigit((unsigned char)**p)) {
        v = v * 10 + (**p - '0');
        (*p)++;
        if (v > 1000000) {
            return 1000000;  // absurd; just keep it ordered
        }
    }
    if (**p == '.') {
        (*p)++;
    }
    return v;
}

int ota_version_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        long x = next_part(&a), y = next_part(&b);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (x < 0) {
            return 0;
        }
    }
    return 0;
}
