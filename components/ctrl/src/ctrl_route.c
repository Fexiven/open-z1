#include "ctrl_route.h"

#include <stdio.h>
#include <string.h>

#include "mkframe.h"

static bool word_is(const uint8_t *line, size_t len, const char *word)
{
    size_t w = strlen(word);
    return len >= w && !memcmp(line, word, w) && (len == w || line[w] == ' ');
}

static bool starts_with(const uint8_t *line, size_t len, const char *prefix)
{
    size_t w = strlen(prefix);
    return len >= w && !memcmp(line, prefix, w);
}

// The next space-separated word from *i: moves *i past the spaces to its
// start and returns its end (both len if none is left).
static size_t next_word(const uint8_t *line, size_t len, size_t *i)
{
    while (*i < len && line[*i] == ' ') {
        (*i)++;
    }
    size_t e = *i;
    while (e < len && line[e] != ' ') {
        e++;
    }
    return e;
}

// Commands the Z1 LPC cannot serve (no SD card, no Wi-Fi) and that this
// firmware does not implement yet. Forwarding them would only produce
// "Could not ..." from the LPC.
static const char *const k_not_yet[] = {
    "cat", "md5sum", "upload", "download", "net", "config-get-all",
};
// Stock factory tools, deliberately left out.
static const char *const k_not_offered[] = {"sn-set", "clearftm"};
// Of those, the ones the app waits on with a LOAD_FINISH/LOAD_ERROR timeout.
static const char *const k_load_reply[] = {
    "ls", "rm", "mv", "mkdir", "cat", "md5sum", "upload", "download", "wlan",
};

static bool in_list(const uint8_t *line, size_t len, const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (word_is(line, len, list[i])) {
            return true;
        }
    }
    return false;
}

// ls, rm, mv and mkdir: splits the arguments, skipping options (-e, -s).
// Spaces in paths are escaped, so arguments are space-separated words.
static bool file_command(const uint8_t *line, size_t off, size_t len, ctrl_route_t *r)
{
    static const struct {
        const char *word;
        ctrl_action_t action;
        size_t args;
    } k_cmds[] = {
        {"ls", CTRL_LOCAL_LS, 1}, {"rm", CTRL_LOCAL_RM, 1}, {"mv", CTRL_LOCAL_MV, 2}, {"mkdir", CTRL_LOCAL_MKDIR, 1}};
    for (size_t c = 0; c < sizeof(k_cmds) / sizeof(k_cmds[0]); c++) {
        if (!word_is(line, len, k_cmds[c].word)) {
            continue;
        }
        r->load_reply = true;  // the app waits for LOAD_FINISH/LOAD_ERROR
        r->line_off = off;
        r->line_len = len;
        for (size_t i = strlen(k_cmds[c].word); i < len;) {
            size_t e = next_word(line, len, &i);
            if (e > i && line[i] == '-') {
                r->ls_sizes |= memchr(line + i, 's', e - i) != NULL;
            } else if (e > i && r->nargs < 2) {
                r->arg_off[r->nargs] = off + i;
                r->arg_len[r->nargs] = e - i;
                r->nargs++;
            } else if (e > i) {
                r->nargs = 3;  // too many
            }
            i = e;
        }
        if (r->nargs != k_cmds[c].args) {
            r->action = CTRL_REJECT;
            r->reason = "wrong arguments";
        } else {
            r->action = k_cmds[c].action;
        }
        return true;
    }
    return false;
}

// config-get, config-set, config-restore, config-default: answered by the
// Mainboard (stock does the same; the LPC has no card). Up to three words
// follow; values may start with '-', so nothing counts as an option.
static bool config_command(const uint8_t *line, size_t off, size_t len, ctrl_route_t *r)
{
    static const char *const k_words[] = {"config-get", "config-set", "config-restore", "config-default"};
    for (size_t c = 0; c < sizeof(k_words) / sizeof(k_words[0]); c++) {
        if (!word_is(line, len, k_words[c])) {
            continue;
        }
        r->action = CTRL_LOCAL_CONFIG;
        r->line_off = off;
        r->line_len = strlen(k_words[c]);
        for (size_t i = strlen(k_words[c]); i < len && r->nargs < 3;) {
            size_t e = next_word(line, len, &i);
            if (e > i) {
                r->arg_off[r->nargs] = off + i;
                r->arg_len[r->nargs] = e - i;
                r->nargs++;
            }
            i = e;
        }
        return true;
    }
    return false;
}

// `wlan`: options -e (send LOAD frames) and -d <anything> (disconnect); the
// other words are the SSID and password (escaped, so never containing a space).
static void wlan_command(const uint8_t *line, size_t off, size_t len, ctrl_route_t *r)
{
    r->action = CTRL_LOCAL_WLAN;
    r->load_reply = true;
    for (size_t i = 4; i < len;) {
        size_t e = next_word(line, len, &i);
        if (e - i == 2 && line[i] == '-' && line[i + 1] == 'e') {
            r->wlan_eof = true;
        } else if (e - i == 2 && line[i] == '-' && line[i + 1] == 'd') {
            r->wlan_off = true;
            size_t w = e;
            e = next_word(line, len, &w);  // and its word ("disconnect")
        } else if (e > i && r->nargs < 2) {
            r->arg_off[r->nargs] = off + i;
            r->arg_len[r->nargs] = e - i;
            r->nargs++;
        } else if (e > i) {
            r->action = CTRL_REJECT;
            r->reason = "wrong arguments";
        }
        i = e;
    }
}

ctrl_route_t ctrl_route(uint8_t type, const uint8_t *payload, size_t n)
{
    ctrl_route_t r = {.action = CTRL_IGNORE};
    if (type == PTYPE_CTRL_SINGLE) {
        if (n == 1 && payload[0] != '\0' && strchr("?!~*\x18\x19\x1a", payload[0])) {
            r.action = CTRL_FORWARD_REALTIME;
        }
        return r;
    }
    // FILE_* is never forwarded: the Z1 LPC would run a FILE_START payload as
    // a console line. Transfers are the Mainboard's own.
    if (type == PTYPE_FILE_START) {
        // Mid-transfer the app ignores everything but FILE_*, so any refusal
        // of a FILE_START must be FILE_CAN.
        r.file_cancel = true;
        size_t len = n;
        while (len && (payload[len - 1] == '\r' || payload[len - 1] == '\n')) {
            len--;
        }
        if (len > 9 && !memcmp(payload, "download ", 9)) {
            r.action = CTRL_FILE_DOWNLOAD;
            r.line_off = 9;
            r.line_len = len - 9;
        } else if (len > 7 && !memcmp(payload, "upload ", 7)) {
            r.action = CTRL_FILE_UPLOAD;
            r.line_off = 7;
            r.line_len = len - 7;
        } else {
            r.action = CTRL_REJECT;
            r.reason = "unknown transfer";
        }
        return r;
    }
    if (type == PTYPE_FILE_PLAYING) {
        r.action = CTRL_LOCAL_PLAYING;
        return r;
    }
    if (type > PTYPE_FILE_START && type <= PTYPE_FILE_RETRY) {
        r.action = CTRL_FILE_FRAME;
        return r;
    }
    if (type != PTYPE_CTRL_MULTI) {
        return r;
    }

    size_t off = 0, len = n;
    while (len && (payload[len - 1] == '\r' || payload[len - 1] == '\n')) {
        len--;
    }
    while (off < len && (payload[off] == ' ' || payload[off] == '\t')) {
        off++;
    }
    const uint8_t *line = payload + off;
    len -= off;
    if (len == 0) {
        return r;
    }
    r.secret = word_is(line, len, "wlan");
    if (word_is(line, len, "version")) {
        r.action = CTRL_LOCAL_VERSION;
    } else if (word_is(line, len, "ftype")) {
        r.action = CTRL_LOCAL_FTYPE;
    } else if (file_command(line, off, len, &r)) {
        // routed by file_command
    } else if (config_command(line, off, len, &r)) {
        // routed by config_command
    } else if (word_is(line, len, "wlan")) {
        wlan_command(line, off, len, &r);
    } else if (word_is(line, len, "sn-get")) {
        r.action = CTRL_LOCAL_SN;
    } else if (word_is(line, len, "time")) {
        // Stock keeps it local: `time <N>` sets the clock without a reply,
        // `time` tells it (stock strings; INFERRED that it is not forwarded).
        r.action = CTRL_LOCAL_TIME;
        r.line_off = off;
        r.line_len = len;
    } else if (len == 14 && !memcmp(line, "upgrade cancel", 14)) {
        r.action = CTRL_LOCAL_UPGRADE_CANCEL;
    } else if (word_is(line, len, "upgrade")) {
        r.action = CTRL_LOCAL_UPGRADE;  // never forwarded: the LPC has no use for it
    } else if (len == 5 && !memcmp(line, "reset", 5)) {
        r.action = CTRL_LOCAL_RESET;
        r.line_off = off;
        r.line_len = len;
    } else if (word_is(line, len, "sys-time")) {
        r.action = CTRL_LOCAL_SYS_TIME;
    } else if (starts_with(line, len, "M485") || starts_with(line, len, "m485")) {
        r.action = CTRL_LOCAL_M485;
    } else if (in_list(line, len, k_not_offered, sizeof(k_not_offered) / sizeof(k_not_offered[0]))) {
        r.action = CTRL_REJECT;
        r.reason = "not offered";
    } else if (in_list(line, len, k_not_yet, sizeof(k_not_yet) / sizeof(k_not_yet[0]))) {
        r.action = CTRL_REJECT;
        r.reason = "command not supported yet";
        r.load_reply = in_list(line, len, k_load_reply, sizeof(k_load_reply) / sizeof(k_load_reply[0]));
    } else if (len > LPC_MAX_TX_PAYLOAD) {
        r.action = CTRL_REJECT;
        r.reason = "line too long";
    } else if (word_is(line, len, "play")) {
        // The LPC then asks us for the file (play_sm.h), so remember which.
        // Spaces in paths are escaped, so the path is the first word.
        size_t p = 4;
        while (p < len && line[p] == ' ') {
            p++;
        }
        size_t e = p;
        while (e < len && line[e] != ' ') {
            e++;
        }
        if (e == p) {
            r.action = CTRL_REJECT;
            r.reason = "play needs a file";
        } else {
            r.action = CTRL_FORWARD_LINE;
            r.line_off = off;
            r.line_len = len;
            r.track_play = true;
            r.path_off = off + p;
            r.path_len = e - p;
        }
    } else {
        r.action = CTRL_FORWARD_LINE;
        r.line_off = off;
        r.line_len = len;
        // The LPC matches these exactly (SerialConsole.cpp), so no arguments.
    }
    return r;
}

int ctrl_version_reply(char *out, size_t cap, const char *lpc, const char *ours)
{
    size_t digits = strspn(ours, "0123456789.");
    while (digits && ours[digits - 1] == '.') {
        digits--;
    }
    int n = snprintf(out, cap, "version = %s.%.*s\n", lpc ? lpc : "0.0.0", (int)digits, ours);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

uint8_t ctrl_ls_end(bool path_ok, const char **text)
{
    *text = path_ok ? "Load directory finished.\r\n" : "";
    return path_ok ? PTYPE_LOAD_FINISH : PTYPE_LOAD_ERROR;
}

size_t ctrl_stop_bytes(const uint8_t *buf, size_t n, uint8_t *out, size_t cap)
{
    size_t k = 0;
    for (size_t i = 0; i + 10 <= n && k < cap; i++) {
        // 86 68 | 00 04 | A1 | byte | crc16 | 55 AA
        const uint8_t *f = buf + i;
        if (f[0] != 0x86 || f[1] != 0x68 || f[2] != 0 || f[3] != 4 || f[4] != PTYPE_CTRL_SINGLE || f[8] != 0x55 ||
            f[9] != 0xAA || (f[5] != '!' && f[5] != 0x18 && f[5] != 0x19)) {
            continue;
        }
        if (mk_crc16(f + 2, 4) == (uint16_t)(f[6] << 8 | f[7])) {
            out[k++] = f[5];
            i += 9;
        }
    }
    return k;
}

int ctrl_diag_rssi(char *out, size_t cap, const uint8_t *in, size_t n, int rssi)
{
    if (n < 2 || in[n - 2] != '}' || in[n - 1] != '\n') {
        return -1;
    }
    int m = snprintf(out, cap, "%.*s|RSSI:%d}\n", (int)(n - 2), (const char *)in, rssi);
    return m < 0 || (size_t)m >= cap ? -1 : m;
}

int ctrl_status_extras(char *out, size_t cap, const uint8_t *in, size_t n, uint32_t sd_used, uint32_t sd_total,
                       unsigned ota_phase, unsigned ota_pct)
{
    size_t gt = n;
    while (gt > 0 && in[gt - 1] != '>') {
        gt--;
    }
    if (gt == 0) {
        return -1;
    }
    gt--;  // at the '>'
    int m = snprintf(out, cap, "%.*s|E:0,0,0,%lu,%lu|OTA:%u,%u%.*s", (int)gt, (const char *)in,
                     (unsigned long)sd_used, (unsigned long)sd_total, ota_phase, ota_pct, (int)(n - gt),
                     (const char *)in + gt);
    return m < 0 || (size_t)m >= cap ? -1 : m;
}

bool ctrl_unescape(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char k_map[] = {0, ' ', '?', '&', '!', '~'};
    if (n >= cap) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = in[i] >= 1 && in[i] <= 5 ? k_map[in[i]] : (char)in[i];
    }
    out[n] = '\0';
    return true;
}
