#pragma once
// Pure Mainboard side of the LPC1768 bootloader's firmware download. The
// bootloader is not in the GPL source; the exchange is from stock's log of a
// real update and mirrors the LPC firmware's own FACTORY_*
// exchange (Kernel::process_factory_command). The bootloader drives it, we only answer:
//
//   LPC FIRM_START (empty), after every LPC reset  us FIRM_START (empty ack), or
//                                                  FIRM_CAN when there is no image
//   LPC FIRM_VIEW {u32 ?, u16 frame size}          us FIRM_VIEW {u32 frames, u16 frame size}
//   LPC FIRM_DATA {u32 n}, n = 1, 2, ... ascending us FIRM_DATA {u32 n, bytes (n-1)*512 ..}
//   LPC FIRM_END (INFERRED), then its new firmware starts (FACTORY_START, ...)
//
// Stock's own Mainboard firmware does exactly this (its dfu_LPC1768 module,
// read for interoperability): START is acked with an empty START every time
// it comes and nothing is sent unasked; the frame size is the one the VIEW
// request names (a VIEW request under 6 bytes is answered with FIRM_CAN); each
// DATA request is answered with the frame number and the bytes at
// (n-1) * size; END from the LPC means done (stock then deletes the image).
// It gives up after 5 s without a frame only before the first DATA request.
// Stock: 436 804 bytes = 854 frames, the last one 68 bytes, about 60 ms each
// (51 s); START was acked twice, 1.6 s apart. Nothing is ever sent unasked.
// Every request is answered from the image as it stands, so a bootloader that
// restarts or asks again for a frame gets the same bytes; the image is never
// changed while a transfer runs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Stock's inactivity limit: 5 s without a FIRM_* frame from the bootloader
// ends the transfer on our side (the image stays, and is served again
// whenever the bootloader asks).
#define LPCFW_SM_TIMEOUT_MS 5000

typedef enum {
    LPCFW_SM_IDLE,
    LPCFW_SM_ACKED,    // START acked, waiting for the first request
    LPCFW_SM_SENDING,  // serving frames
    LPCFW_SM_SERVED,   // every frame from 1 to the last has been served
} lpcfw_sm_state_t;

typedef enum {
    LPCFW_EV_NONE,
    LPCFW_EV_DECLINED,  // FIRM_CAN sent: no image
    LPCFW_EV_STARTED,   // a transfer (re)starts
    LPCFW_EV_PROGRESS,  // pct changed
    LPCFW_EV_DONE,      // complete: the image may go
    LPCFW_EV_FAILED,    // over without completing (why): the image stays
    LPCFW_EV_IGNORED,   // a request we do not answer (why)
} lpcfw_ev_t;

typedef struct {
    lpcfw_sm_state_t state;
    uint32_t size, total;  // the image's bytes and frames
    uint16_t fsize;        // bytes per frame: what the bootloader's VIEW asked for (512 seen)
    uint32_t next;         // frames 1..next have been served
    uint32_t last;         // the last frame served
    uint8_t pct;
    int64_t last_ms;       // the bootloader's last FIRM_* frame
} lpcfw_sm_t;

typedef struct {
    uint8_t type;       // frame to send, 0: none; its payload is head[0..head_len) ...
    uint8_t head[6];
    uint8_t head_len;
    uint32_t data_off;  // ... then data_len bytes of the image from data_off
    uint16_t data_len;
    lpcfw_ev_t ev;
    const char *why;    // EV_FAILED, EV_IGNORED
} lpcfw_out_t;

// A frame from the LPC, any type (others than FIRM_* say its firmware runs).
// image_size: the bytes of the image we can serve now, 0 if none (missing,
// unusable, or we must not start now).
lpcfw_out_t lpcfw_sm_frame(lpcfw_sm_t *s, uint8_t type, const uint8_t *p, size_t n, uint32_t image_size,
                           int64_t now_ms);

// Call regularly: ends a transfer the bootloader stopped asking for.
lpcfw_out_t lpcfw_sm_tick(lpcfw_sm_t *s, int64_t now_ms);

static inline bool lpcfw_sm_active(const lpcfw_sm_t *s)
{
    return s->state != LPCFW_SM_IDLE;
}
