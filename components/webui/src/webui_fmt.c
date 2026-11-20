#include "webui_fmt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "camera_res.h"
#include "machine.h"

typedef struct {
    char *p;
    size_t cap, len;
    bool full;
} jbuf_t;

static void put(jbuf_t *b, const char *fmt, ...)
{
    if (b->full) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->full = true;
        return;
    }
    b->len += (size_t)n;
}

// "key":"value", with value escaped for JSON.
static void put_str(jbuf_t *b, const char *key, const char *v)
{
    put(b, "\"%s\":\"", key);
    for (; *v && !b->full; v++) {
        unsigned char c = (unsigned char)*v;
        if (c == '"' || c == '\\') {
            put(b, "\\%c", c);
        } else if (c < 0x20 || c == 0x7f) {
            put(b, "\\u%04x", c);
        } else {
            put(b, "%c", c);
        }
    }
    put(b, "\",");
}

static void put_bool(jbuf_t *b, const char *key, bool v)
{
    put(b, "\"%s\":%s,", key, v ? "true" : "false");
}

static void put_u32(jbuf_t *b, const char *key, uint32_t v)
{
    put(b, "\"%s\":%lu,", key, (unsigned long)v);
}

// Replaces the ',' after an object's last member with c.
static void close_with(jbuf_t *b, char c)
{
    if (!b->full && b->len > 0 && b->p[b->len - 1] == ',') {
        b->p[b->len - 1] = c;
    } else {
        put(b, "%c", c);
    }
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

// A JSON number as the LPC prints them (%d, %1.1f): -?digits[.digits].
static bool json_number(const char *p, size_t n)
{
    size_t i = p[0] == '-';
    size_t int_start = i;
    while (i < n && is_digit(p[i])) {
        i++;
    }
    if (i == int_start || (p[int_start] == '0' && i - int_start > 1)) {
        return false;
    }
    if (i < n && p[i] == '.') {
        size_t frac = ++i;
        while (i < n && is_digit(p[i])) {
            i++;
        }
        if (i == frac) {
            return false;
        }
    }
    return i == n;
}

// The report's field `name` ("F:0.0,2000.0,100.0") as "F":[0.0,2000.0,100.0],
// anything not a number (nan, inf) as null; nothing if the field is absent.
static void put_numbers(jbuf_t *b, const char *report, const char *name)
{
    char v[96];
    if (!machine_status_field(report, name, v, sizeof(v))) {
        return;
    }
    put(b, "\"%s\":[", name);
    for (const char *p = v;;) {
        while (*p == ' ') {
            p++;
        }
        size_t n = strcspn(p, ",");
        size_t t = n;
        while (t > 0 && p[t - 1] == ' ') {
            t--;
        }
        if (json_number(p, t)) {
            put(b, "%.*s", (int)t, p);
        } else {
            put(b, "null");
        }
        if (!p[n]) {
            break;
        }
        put(b, ",");
        p += n + 1;
    }
    put(b, "],");
}

size_t webui_status_json(const webui_status_t *s, char *out, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    jbuf_t b = {.p = out, .cap = cap};
    char mpos[64] = "", wpos[64] = "", progress[40] = "";
    machine_status_field(s->report, "MPos", mpos, sizeof(mpos));
    machine_status_field(s->report, "WPos", wpos, sizeof(wpos));
    machine_status_field(s->report, "P", progress, sizeof(progress));
    put(&b, "{\"machine\":{");
    put_bool(&b, "fresh", s->fresh);
    put_str(&b, "state", s->state);
    put_str(&b, "mpos", mpos);
    put_str(&b, "wpos", wpos);
    static const char *const fields[] = {"F", "S", "T", "H", "A", "L", "C"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        put_numbers(&b, s->report, fields[i]);
    }
    close_with(&b, '}');
    put(&b, ",\"job\":{");
    put_str(&b, "file", s->job_file);
    put_bool(&b, "serving", s->job_serving);
    put_str(&b, "progress", progress);
    close_with(&b, '}');
    put(&b, ",\"lpc\":{");
    put_str(&b, "version", s->lpc_version);
    put_str(&b, "config", s->lpc_config);
    put_bool(&b, "silent", s->lpc_silent);
    put(&b, "\"status_hz\":%lu.%lu,", (unsigned long)(s->lpc_rate_x10 / 10), (unsigned long)(s->lpc_rate_x10 % 10));
    close_with(&b, '}');
    put(&b, ",\"sd\":{");
    put_bool(&b, "mounted", s->sd_mounted);
    put_u32(&b, "used_mb", s->sd_used_mb);
    put_u32(&b, "total_mb", s->sd_total_mb);
    close_with(&b, '}');
    if (s->cam_res >= 0 && s->cam_res < CAMERA_RES_COUNT) {
        put(&b, ",\"camera\":{\"res\":\"%s\",\"options\":[", camera_res_names[s->cam_res]);
        for (int i = 0; i < CAMERA_RES_COUNT; i++) {
            put(&b, "\"%s\",", camera_res_names[i]);
        }
        close_with(&b, ']');
        put(&b, "}");
    } else {
        put(&b, ",\"camera\":null");
    }
    put(&b, ",\"wifi\":{");
    put_bool(&b, "connected", s->wifi_connected);
    put(&b, "\"rssi\":%d,", s->rssi);
    put_str(&b, "ip", s->ip);
    close_with(&b, '}');
    put(&b, ",\"system\":{");
    put_u32(&b, "uptime_s", s->uptime_s);
    put_str(&b, "reset_reason", s->reset_reason);
    put_str(&b, "version", s->version);
    put_str(&b, "slot", s->slot);
    put_str(&b, "slot_state", s->slot_state);
    put_str(&b, "other_slot", s->other_slot);
    put_str(&b, "other_version", s->other_version);
    put_u32(&b, "heap_free", s->heap_free);
    put_u32(&b, "heap_min", s->heap_min);
    put_u32(&b, "psram_free", s->psram_free);
    put_u32(&b, "psram_min", s->psram_min);
    put(&b, "\"cpu\":[%d,%d],", s->cpu[0], s->cpu[1]);
    close_with(&b, '}');
    put(&b, "}");
    if (b.full) {
        out[0] = '\0';
        return 0;
    }
    return b.len;
}

// Length of a dotted quad (1-3 digits each) at p, or 0.
static size_t ipv4_len(const char *p)
{
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        size_t d = 0;
        while (is_digit(p[i]) && d < 3) {
            i++, d++;
        }
        if (d == 0 || is_digit(p[i])) {
            return 0;
        }
        if (part < 3) {
            if (p[i] != '.') {
                return 0;
            }
            i++;
        }
    }
    return p[i] == '.' && is_digit(p[i + 1]) ? 0 : i;  // a longer dotted number is not an address
}

void webui_redact_ips(char *text, const char *keep)
{
    static const char mask[] = "x.x.x.x";  // no longer than any address, so in place works
    size_t keep_len = strlen(keep);
    char *w = text;
    for (const char *r = text; *r;) {
        size_t n = (r == text || (!is_digit(r[-1]) && r[-1] != '.')) ? ipv4_len(r) : 0;
        if (n == 0) {
            *w++ = *r++;
            continue;
        }
        if (n == keep_len && !memcmp(r, keep, n)) {
            memmove(w, r, n);
            w += n;
        } else {
            memcpy(w, mask, sizeof(mask) - 1);
            w += sizeof(mask) - 1;
        }
        r += n;
    }
    *w = '\0';
}

bool webui_parse_u64(const char *s, uint64_t *v)
{
    uint64_t x = 0;
    size_t i = 0;
    for (; is_digit(s[i]); i++) {
        unsigned d = (unsigned)(s[i] - '0');
        if (i >= 20 || x > (UINT64_MAX - d) / 10) {
            return false;
        }
        x = x * 10 + d;
    }
    if (i == 0 || s[i]) {
        return false;
    }
    *v = x;
    return true;
}
