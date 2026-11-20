#pragma once
// Pure machine side of the app's framed upload (the official app's
// FileUploader):
//
//   app  FILE_START "upload <path>\n", then FILE_MD5 (its 32 hex digits)
//   us   FILE_VIEW  (a request; the app then skips our MD5 reply)
//   app  FILE_VIEW  u32 BE packet count, u16 BE packet size
//   us   FILE_DATA  u32 BE seq (1-based) as a request
//   app  FILE_DATA  u32 BE seq, data
//   ...  until the last packet; we then check and keep the file and send
//        FILE_END, or FILE_CAN if it failed.
//   FILE_CAN from the app: aborted. FILE_RETRY: we repeat our last request.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The app's Wi-Fi packet size (the frame then holds 4 + 8192 bytes).
#define XFER_UL_PACKET_MAX 8192

// Appends n bytes to the file being received; false on a write error.
typedef bool (*xfer_write_fn)(void *ctx, const uint8_t *buf, size_t n);

typedef enum { XFER_UL_IDLE, XFER_UL_WAIT_VIEW, XFER_UL_RECEIVING, XFER_UL_CHECKING } xfer_ul_state_t;

typedef struct {
    xfer_ul_state_t state;
    uint32_t packets, next_seq;
    uint16_t packet_size;
    uint32_t received;  // bytes written
    char md5[33];       // the app's MD5 of the file; "" until it arrives
} xfer_ul_t;

typedef struct {
    uint8_t type;   // frame to send, payload in out (at most 8 bytes); 0: nothing
    size_t len;
    bool complete;  // all data written: check it, then call xfer_ul_finish
    bool failed;    // over without a file: drop what was written
} xfer_ul_out_t;

xfer_ul_out_t xfer_ul_begin(xfer_ul_t *x, uint8_t *out);
xfer_ul_out_t xfer_ul_on_frame(xfer_ul_t *x, uint8_t type, const uint8_t *payload, size_t n, xfer_write_fn write,
                               void *ctx, uint8_t *out);
// ok: the file was checked and kept. Sends FILE_END, else FILE_CAN.
xfer_ul_out_t xfer_ul_finish(xfer_ul_t *x, bool ok, uint8_t *out);
