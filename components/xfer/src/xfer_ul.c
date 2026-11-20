#include "xfer_ul.h"

#include <string.h>

#include "mkframe.h"

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static xfer_ul_out_t request(xfer_ul_t *x, uint8_t *out)
{
    put_u32(out, x->next_seq);
    return (xfer_ul_out_t){.type = PTYPE_FILE_DATA, .len = 4};
}

static xfer_ul_out_t fail(xfer_ul_t *x)
{
    x->state = XFER_UL_IDLE;
    return (xfer_ul_out_t){.type = PTYPE_FILE_CAN, .failed = true};
}

xfer_ul_out_t xfer_ul_begin(xfer_ul_t *x, uint8_t *out)
{
    memset(x, 0, sizeof(*x));
    x->state = XFER_UL_WAIT_VIEW;
    (void)out;
    return (xfer_ul_out_t){.type = PTYPE_FILE_VIEW, .len = 0};  // empty, as stock
}

static bool is_hex32(const uint8_t *p, size_t n)
{
    if (n != 32) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f') || (p[i] >= 'A' && p[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

xfer_ul_out_t xfer_ul_on_frame(xfer_ul_t *x, uint8_t type, const uint8_t *payload, size_t n, xfer_write_fn write,
                               void *ctx, uint8_t *out)
{
    if (x->state == XFER_UL_IDLE || x->state == XFER_UL_CHECKING) {
        return (xfer_ul_out_t){0};
    }
    switch (type) {
    case PTYPE_FILE_CAN:
        x->state = XFER_UL_IDLE;
        return (xfer_ul_out_t){.failed = true};
    case PTYPE_FILE_MD5:
        if (is_hex32(payload, n)) {
            for (size_t i = 0; i < 32; i++) {
                x->md5[i] = (char)(payload[i] | 0x20);  // compared in lowercase
            }
            x->md5[32] = '\0';
        }
        return (xfer_ul_out_t){0};
    case PTYPE_FILE_RETRY:
        if (x->state == XFER_UL_WAIT_VIEW) {
            memcpy(out, "ok\r\n", 4);
            return (xfer_ul_out_t){.type = PTYPE_FILE_VIEW, .len = 4};
        }
        return request(x, out);
    case PTYPE_FILE_VIEW:
        if (x->state != XFER_UL_WAIT_VIEW || n < 6) {
            return (xfer_ul_out_t){0};
        }
        x->packets = get_u32(payload);
        x->packet_size = (uint16_t)(payload[4] << 8 | payload[5]);
        if (x->packet_size == 0 || x->packet_size > XFER_UL_PACKET_MAX) {
            return fail(x);
        }
        x->state = XFER_UL_RECEIVING;
        x->next_seq = 1;
        if (x->packets == 0) {  // an empty file
            x->state = XFER_UL_CHECKING;
            return (xfer_ul_out_t){.complete = true};
        }
        return request(x, out);
    case PTYPE_FILE_DATA: {
        if (x->state != XFER_UL_RECEIVING || n < 4) {
            return (xfer_ul_out_t){0};
        }
        size_t len = n - 4;
        if (get_u32(payload) != x->next_seq) {
            return request(x, out);  // a stale or repeated packet: ask for the one we need
        }
        // Every packet but the last is full; anything else would leave a gap.
        bool last = x->next_seq == x->packets;
        if (len > x->packet_size || (!last && len != x->packet_size)) {
            return fail(x);
        }
        if (len && !write(ctx, payload + 4, len)) {
            return fail(x);
        }
        x->received += (uint32_t)len;
        if (last) {
            x->state = XFER_UL_CHECKING;
            return (xfer_ul_out_t){.complete = true};
        }
        x->next_seq++;
        return request(x, out);
    }
    default:
        return (xfer_ul_out_t){0};
    }
}

xfer_ul_out_t xfer_ul_finish(xfer_ul_t *x, bool ok, uint8_t *out)
{
    x->state = XFER_UL_IDLE;
    if (!ok) {
        return (xfer_ul_out_t){.type = PTYPE_FILE_CAN, .failed = true};
    }
    memcpy(out, "ok\r\n", 4);
    return (xfer_ul_out_t){.type = PTYPE_FILE_END, .len = 4};
}
