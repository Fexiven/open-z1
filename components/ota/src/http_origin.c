#include "http_origin.h"

#include <ctype.h>
#include <string.h>

// Length of host without a ":<digits>" port; 0 if the port is malformed.
static size_t without_port(const char *host)
{
    const char *colon = strchr(host, ':');
    if (!colon) {
        return strlen(host);
    }
    const char *p = colon + 1;
    if (!*p || strlen(p) > 5) {
        return 0;
    }
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }
    return (size_t)(colon - host);
}

static bool is_ipv4(const char *s, size_t n)
{
    int parts = 0, digits = 0, value = 0;
    for (size_t i = 0; i < n; i++) {
        if (isdigit((unsigned char)s[i])) {
            value = value * 10 + (s[i] - '0');
            if (++digits > 3 || value > 255) {
                return false;
            }
        } else if (s[i] == '.' && digits > 0 && parts < 3) {
            parts++;
            digits = value = 0;
        } else {
            return false;
        }
    }
    return parts == 3 && digits > 0;
}

static bool same_name(const char *a, size_t n, const char *b)
{
    if (strlen(b) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

bool http_host_ok(const char *host, const char *own_name)
{
    size_t n = without_port(host);
    if (n == 0) {
        return false;
    }
    if (is_ipv4(host, n)) {
        return true;
    }
    size_t own = strlen(own_name);
    if (own == 0) {
        return false;
    }
    if (n == own + 6 && same_name(host + own, 6, ".local")) {
        n = own;
    }
    return same_name(host, n, own_name);
}

bool http_origin_ok(const char *origin, const char *host)
{
    static const char scheme[] = "http://";
    if (!host[0] || strncmp(origin, scheme, sizeof(scheme) - 1) != 0) {
        return false;
    }
    origin += sizeof(scheme) - 1;
    for (; *origin && *host; origin++, host++) {
        if (tolower((unsigned char)*origin) != tolower((unsigned char)*host)) {
            return false;
        }
    }
    return *origin == *host;  // both ended
}
