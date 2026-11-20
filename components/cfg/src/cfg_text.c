#include "cfg_text.h"

#include <stdio.h>
#include <string.h>

static bool key_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

bool cfg_key_ok(const char *key)
{
    size_t n = strlen(key);
    if (n == 0 || n > 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!key_char(key[i])) {
            return false;
        }
    }
    return true;
}

bool cfg_value_ok(const char *value)
{
    size_t n = strlen(value);
    if (n == 0 || n > 48) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)value[i] <= ' ' || value[i] == '#' || value[i] == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

// Finds key's line. Sets *vs/*ve to its value (empty if the line has none,
// both then at the end of the key).
static bool find(const char *t, size_t n, const char *key, size_t *vs, size_t *ve)
{
    size_t klen = strlen(key);
    for (size_t ls = 0; ls < n;) {
        size_t le = ls;
        while (le < n && t[le] != '\n') {
            le++;
        }
        size_t k = ls;
        while (k < le && blank(t[k])) {
            k++;
        }
        size_t ke = k;
        while (ke < le && !blank(t[ke]) && t[ke] != '#') {
            ke++;
        }
        if (ke - k == klen && !memcmp(t + k, key, klen) && t[k] != '#') {
            size_t v = ke;
            while (v < le && blank(t[v])) {
                v++;
            }
            size_t e = v;
            while (e < le && !blank(t[e]) && t[e] != '#') {
                e++;
            }
            if (e == v) {  // no value: insert right after the key
                v = e = ke;
            }
            *vs = v;
            *ve = e;
            return true;
        }
        ls = le + 1;
    }
    return false;
}

bool cfg_get(const char *text, size_t n, const char *key, char *value, size_t cap)
{
    size_t vs, ve;
    if (!find(text, n, key, &vs, &ve) || ve == vs || ve - vs >= cap) {
        return false;
    }
    memcpy(value, text + vs, ve - vs);
    value[ve - vs] = '\0';
    return true;
}

long cfg_set(const char *text, size_t n, const char *key, const char *value, char *out, size_t cap)
{
    size_t vlen = strlen(value), vs, ve;
    if (find(text, n, key, &vs, &ve)) {
        bool gap = vs == ve && (ve == n || !blank(text[ve]));  // key without value: add a space
        size_t len = vs + gap + vlen + (n - ve);
        if (len > cap) {
            return -1;
        }
        memcpy(out, text, vs);
        if (gap) {
            out[vs] = ' ';
        }
        memcpy(out + vs + gap, value, vlen);
        memcpy(out + vs + gap + vlen, text + ve, n - ve);
        return (long)len;
    }
    bool nl = n && text[n - 1] != '\n';
    int add = snprintf(NULL, 0, "\n%s         %s         # added\n", key, value);
    if (n + nl + (size_t)add + 1 > cap) {
        return -1;
    }
    memcpy(out, text, n);
    if (nl) {
        out[n] = '\n';
    }
    snprintf(out + n + nl, cap - n - nl, "\n%s         %s         # added\n", key, value);
    return (long)(n + nl + (size_t)add);
}

// Calls back for each line the LPC gets; stops when it returns false.
static void each_line(const char *t, size_t n, bool (*fn)(void *ctx, const char *line, size_t len), void *ctx)
{
    for (size_t ls = 0; ls < n;) {
        size_t le = ls;
        while (le < n && t[le] != '\n' && t[le] != '\0') {
            le++;
        }
        if (le > ls && le - ls <= CFG_LINE_MAX && !fn(ctx, t + ls, le - ls)) {
            return;
        }
        ls = le + 1;
    }
}

static bool count_one(void *ctx, const char *line, size_t len)
{
    (void)line, (void)len;
    (*(uint32_t *)ctx)++;
    return true;
}

uint32_t cfg_lpc_long_line(const char *text, size_t n)
{
    uint32_t line = 1;
    for (size_t ls = 0; ls < n; line++) {
        size_t le = ls;
        while (le < n && text[le] != '\n' && text[le] != '\0') {
            le++;
        }
        if (le - ls > CFG_LINE_MAX) {
            return line;
        }
        ls = le + 1;
    }
    return 0;
}

uint32_t cfg_lpc_count(const char *text, size_t n)
{
    uint32_t count = 0;
    each_line(text, n, count_one, &count);
    return count;
}

typedef struct {
    uint32_t skip;
    char *out;
    size_t cap, len;
    uint32_t lines;
} chunk_t;

static bool add_line(void *ctx, const char *line, size_t len)
{
    chunk_t *c = ctx;
    if (c->skip) {
        c->skip--;
        return true;
    }
    if (c->len + len + 1 > c->cap) {
        return false;
    }
    memcpy(c->out + c->len, line, len);
    c->len += len;
    c->out[c->len++] = '\n';
    c->lines++;
    return true;
}

size_t cfg_lpc_chunk(const char *text, size_t n, uint32_t first, char *out, size_t cap, uint32_t *lines)
{
    chunk_t c = {.skip = first ? first - 1 : 0, .out = out, .cap = cap};
    each_line(text, n, add_line, &c);
    *lines = c.lines;
    return c.len;
}
