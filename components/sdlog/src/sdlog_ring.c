#include "sdlog_ring.h"

#include <string.h>

void sdlog_ring_init(sdlog_ring_t *r, char *buf, size_t size)
{
    r->buf = buf;
    r->size = size;
    r->head = 0;
}

void sdlog_ring_put(sdlog_ring_t *r, const char *s, size_t n)
{
    if (n > r->size) {
        s += n - r->size;
        r->head += n - r->size;
        n = r->size;
    }
    size_t at = (size_t)(r->head % r->size);
    size_t first = n < r->size - at ? n : r->size - at;
    memcpy(r->buf + at, s, first);
    memcpy(r->buf, s + first, n - first);
    r->head += n;
}

static char at(const sdlog_ring_t *r, uint64_t i)
{
    return r->buf[i % r->size];
}

static uint64_t oldest(const sdlog_ring_t *r)
{
    return r->head > r->size ? r->head - r->size : 0;
}

static uint64_t next_line(const sdlog_ring_t *r, uint64_t p)
{
    while (p < r->head && at(r, p) != '\n') {
        p++;
    }
    return p < r->head ? p + 1 : p;
}

size_t sdlog_ring_read(const sdlog_ring_t *r, uint64_t *pos, char *out, size_t cap, bool *lost)
{
    uint64_t p = *pos;
    if (p < oldest(r)) {
        *lost = true;
        p = next_line(r, oldest(r));  // the first line there is cut
    }
    size_t n = r->head - p < cap ? (size_t)(r->head - p) : cap;
    size_t at = (size_t)(p % r->size);
    size_t first = n < r->size - at ? n : r->size - at;
    memcpy(out, r->buf + at, first);
    memcpy(out + first, r->buf, n - first);
    *pos = p + n;
    return n;
}

size_t sdlog_last_lines(char *buf, size_t n, unsigned lines, bool cut_start)
{
    size_t start = 0;
    if (cut_start) {
        while (start < n && buf[start] != '\n') {
            start++;
        }
        start = start < n ? start + 1 : n;  // the first line may be partial
    }
    size_t p = n;
    if (p > start && buf[p - 1] == '\n') {
        p--;  // the last line's own end
    }
    unsigned found = 0;
    while (p > start && found < lines) {
        if (buf[p - 1] == '\n' && ++found == lines) {
            break;
        }
        p--;
    }
    if (lines == 0) {
        p = n;
    }
    memmove(buf, buf + p, n - p);
    return n - p;
}

size_t sdlog_whole_lines(const char *buf, size_t n)
{
    while (n > 0 && buf[n - 1] != '\n') {
        n--;
    }
    return n;
}

bool sdlog_line_from_wifi_driver(const char *s, size_t n)
{
    size_t i = 0;
    if (i < n && s[i] == '\033') {  // colour: ESC [ ... m
        while (i < n && s[i] != 'm') {
            i++;
        }
        i++;
    }
    if (i + 3 > n || !strchr("EWIDV", s[i]) || s[i + 1] != ' ' || s[i + 2] != '(') {
        return false;
    }
    while (i < n && s[i] != ')') {
        i++;
    }
    i += 2;  // ") "
    if (i >= n) {
        return false;
    }
    const char *tag = s + i;
    size_t len = 0;
    while (i + len < n && tag[len] != ':' && tag[len] != ' ') {
        len++;
    }
    return (len == 4 && !memcmp(tag, "wifi", 4)) || (len == 8 && !memcmp(tag, "net80211", 8)) ||
           (len >= 3 && !memcmp(tag, "wpa", 3));
}
