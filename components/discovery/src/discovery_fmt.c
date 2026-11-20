#include "discovery_fmt.h"

#include <stdio.h>
#include <string.h>

int discovery_format(char *out, size_t cap, const char *name, const char *ip, unsigned tcp_port, bool busy,
                     const char *state)
{
    if (strchr(name, ',') || strchr(ip, ',') || strchr(state, ',')) {
        return -1;
    }
    int n = snprintf(out, cap, "%s,%s,%u,%d,%s", name, ip, tcp_port, busy ? 1 : 0, state);
    if (n < 0 || (size_t)n >= cap || n > DISCOVERY_MAX_LEN) {
        return -1;
    }
    return n;
}

void discovery_name(char *out, size_t cap, const uint8_t mac[6])
{
    snprintf(out, cap, "Makera_Z1_%02X%02X", mac[4], mac[5]);
}
