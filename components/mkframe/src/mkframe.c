#include "mkframe.h"

#include <string.h>

uint16_t mk_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// The LPC bootloader checks FIRM_* frames with Makera's table-driven
// CRC-16/XMODEM, whose table has two wrong entries (0xE6, 0xE7). Stock sends
// those frames with the same table (verified on all 854 data frames of a
// logic-analyzer capture of a stock update); the LPC app uses the
// correct table. The bitwise CRC equals the table form except where a lookup
// hits those two indices, so patch exactly those lookups.
uint16_t mk_crc16_lpcboot(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)(crc >> 8) ^ data[i];
        uint16_t t;
        if (idx == 0xE6) {
            t = 0x8FD9;
        } else if (idx == 0xE7) {
            t = 0x9FF8;
        } else {
            t = mk_crc16(&idx, 1);  // table entry = CRC of the single index byte
        }
        crc = (uint16_t)(crc << 8) ^ t;
    }
    return crc;
}

static uint16_t frame_crc(uint8_t type, const uint8_t *data, size_t len)
{
    return MK_TYPE_IS_FIRM(type) ? mk_crc16_lpcboot(data, len) : mk_crc16(data, len);
}

size_t mk_frame_encode(uint8_t type, const void *payload, size_t n, uint8_t *out, size_t out_cap)
{
    if (n > MK_MAX_PAYLOAD || out_cap < n + MK_FRAME_OVERHEAD) {
        return 0;
    }
    size_t len = n + 3;
    out[0] = MK_HEADER >> 8;
    out[1] = MK_HEADER & 0xFF;
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)len;
    out[4] = type;
    if (n) {
        memcpy(&out[5], payload, n);
    }
    uint16_t crc = frame_crc(type, &out[2], len);
    out[5 + n] = crc >> 8;
    out[6 + n] = crc & 0xFF;
    out[7 + n] = MK_FOOTER >> 8;
    out[8 + n] = MK_FOOTER & 0xFF;
    return n + MK_FRAME_OVERHEAD;
}

void mk_parser_init(mk_parser_t *p, uint8_t *buf, size_t cap, mk_stream_t mode)
{
    memset(p, 0, sizeof(*p));
    p->buf = buf;
    p->cap = cap;
    p->mode = mode;
}

void mk_parser_drop_partial(mk_parser_t *p)
{
    if (mk_parser_busy(p)) {
        p->bad_frame++;
    }
    memset(p->buf, 0, p->have);  // it may have held part of a password
    p->have = 0;
    p->need = 0;
    p->skip = 0;
}

// Discards the frame at the front whose total length is `total`, including
// bytes not received yet.
static void drop_front(mk_parser_t *p, size_t total)
{
    if (total <= p->have) {
        memmove(p->buf, p->buf + total, p->have - total);
        p->have -= total;
    } else {
        p->skip = total - p->have;
        p->have = 0;
    }
    p->need = 0;
}

static void resync(mk_parser_t *p)
{
    // Drop the first byte, then skip to the next possible header (a trailing
    // lone 0x86 is kept, since its 0x68 may still be on the way).
    size_t i = 1;
    while (i < p->have &&
           !(p->buf[i] == (MK_HEADER >> 8) && (i + 1 == p->have || p->buf[i + 1] == (MK_HEADER & 0xFF)))) {
        i++;
    }
    memmove(p->buf, p->buf + i, p->have - i);
    p->have -= i;
    p->need = 0;
}

void mk_parser_feed(mk_parser_t *p, const uint8_t *data, size_t len, mk_frame_cb_t cb, void *ctx)
{
    while (len) {
        if (p->skip) {
            size_t drop = len < p->skip ? len : p->skip;
            p->skip -= drop;
            data += drop;
            len -= drop;
            continue;
        }
        size_t room = p->cap - p->have;
        size_t take = len < room ? len : room;
        memcpy(p->buf + p->have, data, take);
        p->have += take;
        data += take;
        len -= take;

        for (;;) {
            if (p->have < 2) {
                break;
            }
            if (p->buf[0] != (MK_HEADER >> 8) || p->buf[1] != (MK_HEADER & 0xFF)) {
                resync(p);
                continue;
            }
            if (p->have < 4) {
                break;
            }
            if (!p->need) {
                size_t flen = ((size_t)p->buf[2] << 8) | p->buf[3];
                size_t total = flen + 6;
                if (flen < 3) {
                    p->bad_frame++;
                    resync(p);
                    continue;
                }
                if (total > p->cap) {
                    p->bad_frame++;
                    if (p->mode == MK_RELIABLE) {
                        drop_front(p, total);
                        if (p->skip) {
                            break;  // the rest of the frame is still to come
                        }
                    } else {
                        resync(p);
                    }
                    continue;
                }
                p->need = total;
            }
            if (p->have < p->need) {
                break;
            }
            size_t total = p->need;
            size_t n = total - MK_FRAME_OVERHEAD;
            uint16_t footer = ((uint16_t)p->buf[total - 2] << 8) | p->buf[total - 1];
            uint16_t crc = ((uint16_t)p->buf[total - 4] << 8) | p->buf[total - 3];
            if (footer != MK_FOOTER) {
                p->bad_frame++;
                resync(p);
                continue;
            }
            if (crc != frame_crc(p->buf[4], &p->buf[2], n + 3)) {
                p->bad_crc++;
                if (p->mode == MK_RELIABLE) {
                    drop_front(p, total);  // boundaries are sound (footer matched)
                } else {
                    // A corrupted length byte makes the claimed frame swallow
                    // the next one, so rescan instead of discarding `total` bytes.
                    resync(p);
                }
                continue;
            }
            cb(p->buf[4], &p->buf[5], n, ctx);
            drop_front(p, total);
        }
    }
}
