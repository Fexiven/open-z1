#pragma once
// Pure machine side of the app's framed download (Carvera Controller
// XMODEM.recv; stock log: MD5 first, the app cancels when its copy matches):
//
//   app  FILE_START "download <path>\n"
//   us   FILE_MD5   32 lowercase hex chars
//   app  FILE_CAN   (has it already: done)  or  FILE_VIEW (empty)
//   us   FILE_VIEW  u32 BE packet count, u16 BE packet size
//   app  FILE_DATA  u32 BE seq (1-based)
//   us   FILE_DATA  u32 BE seq, data
//   ...  until the app sends FILE_END after the last packet.
//   FILE_RETRY from the app: resend our last frame. FILE_MD5 (empty): resend the MD5.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Reads up to n bytes at offset; returns the count read, or -1 on error.
typedef int (*xfer_read_fn)(void *ctx, uint32_t offset, uint8_t *buf, size_t n);

typedef enum { XFER_DL_IDLE, XFER_DL_OFFERED, XFER_DL_SENDING } xfer_dl_state_t;

typedef struct {
    xfer_dl_state_t state;
    uint32_t size;
    uint16_t packet_size;
    uint32_t packets;
    char md5[33];
    uint8_t last_type;  // our last frame, for FILE_RETRY
    uint32_t last_seq;
} xfer_dl_t;

typedef struct {
    uint8_t type;  // 0: nothing to send
    size_t len;
    bool done;     // the transfer is over (finished, cancelled or failed)
} xfer_out_t;

// What stock sends as the MD5 when it does not know a file's hash. It never
// matches the app's cache, so the app always downloads the file.
#define XFER_DL_MD5_UNKNOWN "default_md5_hash_value_32_bytes_"

// Starts offering a file; writes the FILE_MD5 frame payload into out.
// md5_hex: 32 lowercase hex digits of the file's content, or
// XFER_DL_MD5_UNKNOWN. out (cap bytes) must hold max(32, 4 + packet_size)
// bytes, or begin refuses the transfer (FILE_CAN, done). Pass the same buffer
// to every xfer_dl_on_frame call.
xfer_out_t xfer_dl_begin(xfer_dl_t *x, uint32_t size, const char *md5_hex, uint16_t packet_size, uint8_t *out,
                         size_t cap);

// Handles one FILE_* frame from the app; may write a reply payload into out
// (the buffer given to xfer_dl_begin).
xfer_out_t xfer_dl_on_frame(xfer_dl_t *x, uint8_t type, const uint8_t *payload, size_t n, xfer_read_fn read,
                            void *ctx, uint8_t *out);
