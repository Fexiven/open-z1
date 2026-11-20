#include "xfer_dl.h"

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

static xfer_out_t emit_md5(xfer_dl_t *x, uint8_t *out)
{
    memcpy(out, x->md5, 32);
    x->last_type = PTYPE_FILE_MD5;
    return (xfer_out_t){.type = PTYPE_FILE_MD5, .len = 32};
}

static xfer_out_t emit_view(xfer_dl_t *x, uint8_t *out)
{
    put_u32(out, x->packets);
    out[4] = (uint8_t)(x->packet_size >> 8);
    out[5] = (uint8_t)x->packet_size;
    x->last_type = PTYPE_FILE_VIEW;
    return (xfer_out_t){.type = PTYPE_FILE_VIEW, .len = 6};
}

static xfer_out_t emit_data(xfer_dl_t *x, uint32_t seq, xfer_read_fn read, void *ctx, uint8_t *out)
{
    uint32_t offset = (seq - 1) * (uint32_t)x->packet_size;
    uint32_t want = x->size - offset < x->packet_size ? x->size - offset : x->packet_size;
    int got = want ? read(ctx, offset, out + 4, want) : 0;
    if (got != (int)want) {
        // A short or failed read would hand the app a corrupt file; stop instead.
        x->state = XFER_DL_IDLE;
        return (xfer_out_t){.type = PTYPE_FILE_CAN, .done = true};
    }
    put_u32(out, seq);
    x->last_type = PTYPE_FILE_DATA;
    x->last_seq = seq;
    return (xfer_out_t){.type = PTYPE_FILE_DATA, .len = 4 + want};
}

static xfer_out_t on_data(xfer_dl_t *x, const uint8_t *payload, size_t n, xfer_read_fn read, void *ctx, uint8_t *out)
{
    if (x->state != XFER_DL_SENDING || n < 4) {
        return (xfer_out_t){0};
    }
    uint32_t seq = get_u32(payload);
    if (seq == 0 || seq > x->packets) {
        return (xfer_out_t){0};  // nonsense request: the app will retry or give up
    }
    return emit_data(x, seq, read, ctx, out);
}

xfer_out_t xfer_dl_begin(xfer_dl_t *x, uint32_t size, const char *md5_hex, uint16_t packet_size, uint8_t *out,
                         size_t cap)
{
    memset(x, 0, sizeof(*x));
    x->packet_size = packet_size ? packet_size : 1;
    size_t need = 4 + (size_t)x->packet_size;
    if (cap < (need > 32 ? need : 32)) {  // FILE_MD5 is 32 bytes, FILE_DATA 4 + packet_size
        return (xfer_out_t){.type = PTYPE_FILE_CAN, .done = true};  // state stays IDLE
    }
    x->size = size;
    // An empty file is still one (empty) packet: the app always asks for seq 1.
    x->packets = size ? (size + x->packet_size - 1) / x->packet_size : 1;
    memcpy(x->md5, md5_hex, 32);
    x->md5[32] = '\0';
    x->state = XFER_DL_OFFERED;
    return emit_md5(x, out);
}

xfer_out_t xfer_dl_on_frame(xfer_dl_t *x, uint8_t type, const uint8_t *payload, size_t n, xfer_read_fn read,
                            void *ctx, uint8_t *out)
{
    if (x->state == XFER_DL_IDLE) {
        return (xfer_out_t){.done = true};
    }
    switch (type) {
    case PTYPE_FILE_CAN:
    case PTYPE_FILE_END:
        x->state = XFER_DL_IDLE;
        return (xfer_out_t){.done = true};
    case PTYPE_FILE_MD5:
        return emit_md5(x, out);
    case PTYPE_FILE_VIEW:
        x->state = XFER_DL_SENDING;
        return emit_view(x, out);
    case PTYPE_FILE_DATA:
        return on_data(x, payload, n, read, ctx, out);
    case PTYPE_FILE_RETRY:
        if (x->last_type == PTYPE_FILE_DATA) {
            return emit_data(x, x->last_seq, read, ctx, out);
        }
        return x->last_type == PTYPE_FILE_VIEW ? emit_view(x, out) : emit_md5(x, out);
    default:
        return (xfer_out_t){0};
    }
}
