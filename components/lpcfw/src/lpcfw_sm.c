#include "lpcfw_sm.h"

#include "mkframe.h"

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24), b[1] = (uint8_t)(v >> 16), b[2] = (uint8_t)(v >> 8), b[3] = (uint8_t)v;
}

static lpcfw_out_t ended(lpcfw_sm_t *s, lpcfw_ev_t ev, const char *why)
{
    s->state = LPCFW_SM_IDLE;
    return (lpcfw_out_t){.ev = ev, .why = why};
}

// Takes over the image for a transfer; frames 1..next count as served.
static void begin(lpcfw_sm_t *s, lpcfw_sm_state_t st, uint32_t size, uint32_t next, uint16_t fsize)
{
    s->state = st;
    s->size = size;
    s->fsize = fsize;
    s->total = (size + fsize - 1) / fsize;
    s->next = next;
    s->last = 0;
    s->pct = (uint8_t)(((uint64_t)next * 100 + s->total / 2) / s->total);
}

lpcfw_out_t lpcfw_sm_frame(lpcfw_sm_t *s, uint8_t type, const uint8_t *p, size_t n, uint32_t image_size,
                           int64_t now_ms)
{
    lpcfw_out_t o = {0};
    bool firm = type >= PTYPE_FIRM_START && type <= PTYPE_FIRM_CAN;
    if (!firm) {
        // The LPC firmware is running: the bootloader is done.
        if (s->state == LPCFW_SM_SERVED) {
            return ended(s, LPCFW_EV_DONE, NULL);
        }
        if (s->state != LPCFW_SM_IDLE) {
            return ended(s, LPCFW_EV_FAILED, "the motion controller left its bootloader");
        }
        return o;
    }
    s->last_ms = now_ms;
    if (type == PTYPE_FIRM_CAN) {
        // The bootloader gives up (or, idle, ends its own wait for an answer).
        return s->state != LPCFW_SM_IDLE ? ended(s, LPCFW_EV_FAILED, "the bootloader cancelled") : o;
    }
    if (type == PTYPE_FIRM_END) {
        if (s->state == LPCFW_SM_SERVED) {
            return ended(s, LPCFW_EV_DONE, NULL);
        }
        return s->state != LPCFW_SM_IDLE ? ended(s, LPCFW_EV_FAILED, "the bootloader ended before the last frame") : o;
    }
    if (!image_size) {
        bool was = s->state != LPCFW_SM_IDLE;
        s->state = LPCFW_SM_IDLE;
        if (type == PTYPE_FIRM_START) {
            o.type = PTYPE_FIRM_CAN;  // the bootloader then starts the LPC firmware
        }
        o.ev = was ? LPCFW_EV_FAILED : type == PTYPE_FIRM_START ? LPCFW_EV_DECLINED : LPCFW_EV_IGNORED;
        o.why = was ? "the image is gone" : type == PTYPE_FIRM_START ? NULL : "a request without an image";
        return o;
    }
    if (s->state != LPCFW_SM_IDLE && image_size != s->size) {
        return ended(s, LPCFW_EV_FAILED, "the image changed");  // never happens: it is not replaced meanwhile
    }
    switch (type) {
    case PTYPE_FIRM_START:
        // Acked every time: stock's first ack went unanswered and the
        // bootloader asked again. A START mid-transfer is a restart.
        begin(s, LPCFW_SM_ACKED, image_size, 0, LPC_FIRM_PACKET);
        o.type = PTYPE_FIRM_START;
        o.ev = LPCFW_EV_STARTED;
        return o;
    case PTYPE_FIRM_VIEW: {
        uint16_t fsize = n >= 6 ? (uint16_t)(p[4] << 8 | p[5]) : 0;
        if (fsize == 0 || fsize > LPC_FIRM_PACKET) {
            // Stock answers a VIEW request without a size with FIRM_CAN.
            bool was = s->state != LPCFW_SM_IDLE;
            s->state = LPCFW_SM_IDLE;
            o.type = PTYPE_FIRM_CAN;
            o.ev = was ? LPCFW_EV_FAILED : LPCFW_EV_IGNORED;
            o.why = "a view request without a usable frame size";
            return o;
        }
        bool adopt = s->state == LPCFW_SM_IDLE;  // mid-way through a transfer begun before our reboot
        begin(s, LPCFW_SM_ACKED, image_size, 0, fsize);
        if (adopt) {
            o.ev = LPCFW_EV_STARTED;
        }
        o.type = PTYPE_FIRM_VIEW;
        put32(o.head, s->total);
        o.head[4] = (uint8_t)(fsize >> 8), o.head[5] = (uint8_t)fsize;
        o.head_len = 6;
        return o;
    }
    case PTYPE_FIRM_DATA: {
        if (n < 4) {
            o.ev = LPCFW_EV_IGNORED;
            o.why = "a short frame request";
            return o;
        }
        uint32_t f = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        uint16_t fsize = s->state != LPCFW_SM_IDLE ? s->fsize : LPC_FIRM_PACKET;
        uint32_t total = (image_size + fsize - 1) / fsize;
        if (f < 1 || f > total) {
            o.ev = LPCFW_EV_IGNORED;
            o.why = "a frame number outside the image";
            return o;
        }
        if (s->state == LPCFW_SM_IDLE) {
            // Mid-way through a transfer begun before our reboot: the frames
            // before this one came from the same image then.
            begin(s, LPCFW_SM_SENDING, image_size, f - 1, fsize);
            o.ev = LPCFW_EV_STARTED;
        }
        if (f == s->next + 1) {
            s->next = f;
        }
        s->last = f;
        s->state = s->next == s->total ? LPCFW_SM_SERVED : LPCFW_SM_SENDING;
        o.type = PTYPE_FIRM_DATA;
        put32(o.head, f);
        o.head_len = 4;
        o.data_off = (f - 1) * s->fsize;
        uint32_t left = s->size - o.data_off;
        o.data_len = (uint16_t)(left < s->fsize ? left : s->fsize);
        uint8_t pct = (uint8_t)(((uint64_t)s->next * 100 + s->total / 2) / s->total);  // rounded, as stock
        if (pct != s->pct && o.ev == LPCFW_EV_NONE) {
            o.ev = LPCFW_EV_PROGRESS;
        }
        s->pct = pct;
        return o;
    }
    default:
        return o;
    }
}

lpcfw_out_t lpcfw_sm_tick(lpcfw_sm_t *s, int64_t now_ms)
{
    if (s->state != LPCFW_SM_IDLE && now_ms - s->last_ms > LPCFW_SM_TIMEOUT_MS) {
        return ended(s, LPCFW_EV_FAILED, "no request from the bootloader for 5 s");
    }
    return (lpcfw_out_t){0};
}
