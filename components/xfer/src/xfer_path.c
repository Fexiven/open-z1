#include "xfer_path.h"

#include <stdio.h>
#include <string.h>

#include "mkframe.h"

static bool bad_segment(const char *seg, size_t len)
{
    return len == 0 || (len == 1 && seg[0] == '.') || (len == 2 && seg[0] == '.' && seg[1] == '.');
}

bool xfer_path_decode(const char *in, size_t n, char *out, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        switch (c) {
        case 0x01: c = ' '; break;
        case 0x02: c = '?'; break;
        case 0x03: c = '&'; break;
        case 0x04: c = '!'; break;
        case 0x05: c = '~'; break;
        default: break;
        }
        if ((unsigned char)c < 0x20 || c == 0x7f || c == '\\' || len + 1 >= cap || len >= XFER_PATH_MAX) {
            return false;
        }
        out[len++] = c;
    }
    out[len] = '\0';

    if (strncmp(out, "/sd/", 4) != 0) {
        return false;
    }
    // Every segment after "/sd" must be a real name.
    const char *seg = out + 4;
    for (;;) {
        const char *slash = strchr(seg, '/');
        size_t seg_len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (bad_segment(seg, seg_len)) {
            return false;
        }
        if (!slash) {
            return true;
        }
        seg = slash + 1;
    }
}

static bool same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) {
            return false;
        }
    }
    return *a == *b;
}

// FAT matches names without case, drops trailing dots and spaces, and also
// answers to 8.3 aliases such as CONFIG~1.DEF, so a name that looks harmless
// can still reach a protected file. Such segments are refused outright.
static bool segment_ok(const char *seg, size_t len)
{
    if (seg[0] == '.' || seg[len - 1] == '.' || seg[len - 1] == ' ') {
        return false;  // hidden (the sidecar folders), or FAT would strip the end
    }
    for (size_t i = 0; i + 1 < len; i++) {
        if (seg[i] == '~' && seg[i + 1] >= '0' && seg[i + 1] <= '9') {
            return false;
        }
    }
    return true;
}

bool xfer_path_writable(const char *path)
{
    static const char *const k_protected[] = {"/sd/gcodes", "/sd/config.txt", "/sd/config.default",
                                              "/sd/factory.ini", "/sd/firmware.bin", "/sd/lpc1768.bin"};
    if (strncmp(path, "/sd/", 4) != 0) {
        return false;
    }
    for (const char *seg = path + 4;;) {
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (len == 0 || !segment_ok(seg, len)) {
            return false;
        }
        if (!slash) {
            break;
        }
        seg = slash + 1;
    }
    for (size_t i = 0; i < sizeof(k_protected) / sizeof(k_protected[0]); i++) {
        if (same_ci(path, k_protected[i])) {
            return false;
        }
    }
    return true;
}

bool xfer_sidecar(const char *path, const char *kind, char *out, size_t cap)
{
    static const char k_gcodes[] = "/sd/gcodes/";
    int n;
    if (!strncmp(path, k_gcodes, sizeof(k_gcodes) - 1)) {
        n = snprintf(out, cap, "/sd/gcodes/%s/%s", kind, path + sizeof(k_gcodes) - 1);
    } else {
        n = snprintf(out, cap, "/sd/%s/%s", kind, strrchr(path, '/') + 1);
    }
    return n > 0 && (size_t)n < cap;
}

uint16_t xfer_path_lpc_crc(const char *in, size_t n)
{
    static const char k_map[] = {0, ' ', '?', '*', '!', '~'};  // the LPC's shift_parameter
    char name[XFER_PATH_MAX + 1];
    if (n > XFER_PATH_MAX) {
        n = XFER_PATH_MAX;  // too long to be a valid path anyway: xfer_path_decode refuses it
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)in[i];
        name[i] = ch >= 1 && ch <= 5 ? k_map[ch] : (char)ch;
    }
    return mk_crc16((const uint8_t *)name, n);
}
