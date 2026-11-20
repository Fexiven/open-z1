#include "play_sm.h"

#include <string.h>

static void put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

static void put32(uint8_t *b, uint32_t v)
{
    put16(b, (uint16_t)(v >> 16));
    put16(b + 2, (uint16_t)v);
}

static uint16_t get16(const uint8_t *b) { return (uint16_t)(b[0] << 8 | b[1]); }
static uint32_t get32(const uint8_t *b) { return (uint32_t)get16(b) << 16 | get16(b + 2); }

static play_out_t cancel(play_sm_t *p)
{
    p->state = PLAY_IDLE;
    return (play_out_t){.type = PTYPE_PLAY_CAN, .close = true};
}

// Finds the end of the line starting at off. Sets *len to its content length
// (bytes before '\n') and *next to where the following line starts. Returns
// false at end of file (no line there) or on a read error (*err).
static bool next_line(play_read_fn read, void *ctx, uint32_t off, uint32_t *len, uint32_t *next, bool *err)
{
    uint8_t buf[256];
    uint32_t pos = off;
    for (;;) {
        int n = read(ctx, pos, buf, sizeof(buf));
        if (n < 0) {
            *err = true;
            return false;
        }
        if (n == 0) {  // end of file: a last line without '\n', or nothing
            *len = pos - off;
            *next = pos;
            return pos > off;
        }
        const uint8_t *nl = memchr(buf, '\n', (size_t)n);
        if (nl) {
            *len = pos + (uint32_t)(nl - buf) - off;
            *next = off + *len + 1;
            return true;
        }
        pos += (uint32_t)n;
    }
}

typedef enum { SKIP_STOPPED, SKIP_EOF, SKIP_ERROR } skip_t;

// Skips lines until p->line reaches target or max_bytes were scanned
// (SKIP_STOPPED), or the file ends first.
static skip_t skip_lines(play_sm_t *p, play_read_fn read, void *ctx, uint32_t target, uint32_t max_bytes)
{
    uint32_t start = p->off;
    while (p->line < target && p->off - start < max_bytes) {
        uint32_t len, next;
        bool err = false;
        if (!next_line(read, ctx, p->off, &len, &next, &err)) {
            return err ? SKIP_ERROR : SKIP_EOF;
        }
        p->line++;
        p->off = next;
    }
    return SKIP_STOPPED;
}

static bool is_letter(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_number(uint8_t c)
{
    return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+';
}

// A line longer than the LPC takes, without comments ("(...)" and ";..."),
// blanks and CR: the LPC's G-code parser reads letters and numbers without
// separators (Gcode.cpp). Only for G-code: the code must be words of a letter
// and a number ("G1 X-2.5 F300"); a console line ("buffer ...", "M117 text")
// would change meaning without its blanks. Writes it to out; returns its
// length, -1 if the code is longer than LPC_LINE_MAX or is not G-code, -2 on
// a read error.
static int compact_line(play_read_fn read, void *ctx, uint32_t off, uint32_t len, uint8_t *out)
{
    int n = 0;
    bool paren = false;
    uint8_t buf[64];
    for (uint32_t done = 0; done < len;) {
        uint32_t want = len - done < sizeof(buf) ? len - done : sizeof(buf);
        if (read(ctx, off + done, buf, want) != (int)want) {
            return -2;
        }
        for (uint32_t i = 0; i < want; i++) {
            uint8_t c = buf[i];
            if (paren) {
                paren = c != ')';
            } else if (c == '(') {
                paren = true;
            } else if (c == ';') {
                return n == 0 || is_number(out[n - 1]) ? n : -1;  // the rest is a comment
            } else if (c != ' ' && c != '\t' && c != '\r') {
                // A letter starts a word (after a number, or first); a number
                // follows a letter or continues one. Anything else: not G-code.
                bool ok = is_letter(c) ? n == 0 || is_number(out[n - 1]) : is_number(c) && n > 0;
                if (!ok || n == LPC_LINE_MAX) {
                    return -1;
                }
                out[n++] = c;
            }
        }
        done += want;
    }
    return n == 0 || is_number(out[n - 1]) ? n : -1;  // a word must end with its number
}

// Fills one PLAY_DATA reply from the current position with whole lines.
static play_out_t data_reply(play_sm_t *p, uint16_t max_lines, play_read_fn read, void *ctx, uint8_t *out)
{
    p->chunk_line = p->line;
    p->chunk_off = p->off;
    uint8_t *data = out + 6;
    size_t len = 0;
    uint16_t lines = 0;
    while (lines < max_lines) {
        uint32_t llen, next;
        bool err = false;
        if (!next_line(read, ctx, p->off, &llen, &next, &err)) {
            if (err) {
                return cancel(p);  // a gap would hand the LPC a corrupt job
            }
            break;
        }
        // A long line goes without comments and blanks (stock sends it empty,
        // silently); one with more code than the LPC takes stops the job.
        size_t need = (llen <= LPC_LINE_MAX ? llen : LPC_LINE_MAX) + 1;
        if (len + need > PLAY_DATA_MAX) {
            break;
        }
        if (llen <= LPC_LINE_MAX) {
            if (llen && read(ctx, p->off, data + len, llen) != (int)llen) {
                return cancel(p);
            }
            len += llen;
        } else {
            int c = compact_line(read, ctx, p->off, llen, data + len);
            if (c < 0) {
                play_out_t o = cancel(p);
                o.too_long = c == -1;
                o.too_long_line = p->line;
                return o;
            }
            len += (size_t)c;
        }
        data[len++] = '\n';
        lines++;
        p->line++;
        p->off = next;
    }
    if (lines == 0) {
        return (play_out_t){.type = PTYPE_PLAY_END};
    }
    put16(out, p->crc);
    put32(out + 2, p->chunk_line);
    return (play_out_t){.type = PTYPE_PLAY_DATA, .len = 6 + len};
}

void play_lpc_reset(play_sm_t *p)
{
    p->state = PLAY_IDLE;
    p->seeking = false;
    p->has_pending = false;
}

void play_arm(play_sm_t *p, uint16_t crc, uint32_t now_ms)
{
    p->pending_crc = crc;
    p->pending_ms = now_ms;
    p->has_pending = true;
}

bool play_serving(const play_sm_t *p)
{
    return p->state == PLAY_OPENING || p->state == PLAY_PLAYING;
}

play_out_t play_opened(play_sm_t *p, uint32_t size, uint8_t *out)
{
    if (p->opening == PLAY_FILE_PENDING) {
        p->has_pending = false;
    }
    p->has_last = true;
    p->last_crc = p->crc;
    p->state = PLAY_PLAYING;
    p->size = size;
    put16(out, p->crc);
    put32(out + 2, size);
    return (play_out_t){.type = PTYPE_PLAY_VIEW, .len = 6};
}

play_out_t play_open_failed(play_sm_t *p)
{
    if (p->opening == PLAY_FILE_PENDING) {
        p->has_pending = false;
    }
    return cancel(p);
}

play_out_t play_goto_step(play_sm_t *p, play_read_fn read, void *ctx, uint8_t *out)
{
    skip_t r = skip_lines(p, read, ctx, p->goto_target, PLAY_GOTO_STEP);
    if (r == SKIP_ERROR) {
        p->seeking = false;
        return cancel(p);
    }
    // Past the end of the file the LPC gets the line count reached and
    // reports the goto as failed.
    p->seeking = r == SKIP_STOPPED && p->line < p->goto_target;
    p->chunk_line = p->line;
    p->chunk_off = p->off;
    put16(out, p->crc);
    put32(out + 2, p->line);
    put32(out + 6, p->off);
    return (play_out_t){.type = PTYPE_GOTO_LINES, .len = 10, .more = p->seeking};
}

play_out_t play_on_lpc(play_sm_t *p, uint8_t type, const uint8_t *payload, size_t n, play_read_fn read, void *ctx,
                       uint8_t *out, uint32_t now_ms)
{
    switch (type) {
    case PTYPE_PLAY_START: {
        // The LPC sends this only when idle (Player::play_command), so any job
        // still open here ended without us hearing of it.
        uint16_t crc = n >= 2 ? get16(payload) : 0;
        if (p->has_pending && now_ms - p->pending_ms > PLAY_ARM_TTL_MS) {
            p->has_pending = false;
        }
        play_file_t f = n < 2                                    ? PLAY_FILE_NONE
                        : p->has_pending && crc == p->pending_crc ? PLAY_FILE_PENDING
                        : p->has_last && crc == p->last_crc       ? PLAY_FILE_LAST
                                                                  : PLAY_FILE_NONE;
        if (f == PLAY_FILE_NONE) {
            // INFERRED: stock logs "start check failed" here; a PLAY_CAN is
            // harmless to an LPC that is not playing yet.
            return cancel(p);
        }
        p->state = PLAY_OPENING;
        p->opening = f;
        p->crc = crc;
        p->line = p->off = p->chunk_line = p->chunk_off = 0;
        p->seeking = false;
        return (play_out_t){.close = true, .open = f};
    }
    case PTYPE_PLAY_DATA:
        if (p->state != PLAY_PLAYING || n < 8 || get16(payload) != p->crc) {
            return cancel(p);  // INFERRED: stock logs "data check failed"
        }
        {
            uint32_t want = get32(payload + 2);
            uint16_t free_slots = get16(payload + 6);
            if (want == p->chunk_line) {  // the LPC missed our last reply
                p->line = p->chunk_line;
                p->off = p->chunk_off;
            } else if (want != p->line) {  // any other jump: count lines from the start
                if (want < p->line) {
                    p->line = 0;
                    p->off = 0;
                }
                if (skip_lines(p, read, ctx, want, UINT32_MAX) == SKIP_ERROR) {
                    return cancel(p);
                }
            }
            if (free_slots == 0) {
                return (play_out_t){0};
            }
            return data_reply(p, free_slots, read, ctx, out);
        }
    case PTYPE_GOTO_START:
        if (p->state != PLAY_PLAYING || n < 6 || get16(payload) != p->crc) {
            return (play_out_t){0};  // the LPC times out and reports the goto failed
        }
        {
            uint32_t line = get32(payload + 2);
            p->goto_target = line ? line - 1 : 0;  // lines before line N
            p->line = 0;
            p->off = 0;
            p->seeking = true;
            return play_goto_step(p, read, ctx, out);
        }
    case PTYPE_PLAY_CAN:  // abort
    case PTYPE_PLAY_END:  // the job finished (Player::cleanup_after_play_finish)
        p->state = PLAY_IDLE;
        return (play_out_t){.close = true};
    default:
        return (play_out_t){0};
    }
}
