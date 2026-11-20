#include "machine.h"

#include <ctype.h>
#include <string.h>

bool machine_status_state(const char *s, size_t n, char *out, size_t cap)
{
    if (n < 2 || s[0] != '<' || cap == 0) {
        return false;
    }
    size_t len = 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        // '|' in current Grbl/Smoothie reports, ',' in the older "<Idle,MPos:...>" form.
        if (c == '|' || c == '>' || c == ',') {
            if (len == 0) {
                return false;
            }
            out[len] = '\0';
            return true;
        }
        if (!(isalnum((unsigned char)c) || c == ':') || len + 1 >= cap) {
            return false;
        }
        out[len++] = c;
    }
    return false;  // no terminator: truncated report
}

bool machine_status_field(const char *s, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = strchr(s, '|'); p; p = strchr(p + 1, '|')) {
        if (strncmp(p + 1, name, nl) != 0 || p[1 + nl] != ':') {
            continue;
        }
        const char *v = p + 2 + nl;
        size_t len = strcspn(v, "|>");
        if (len >= cap) {
            return false;
        }
        memcpy(out, v, len);
        out[len] = '\0';
        return true;
    }
    return false;
}
