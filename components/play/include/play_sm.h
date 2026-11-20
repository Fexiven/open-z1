#pragma once
// Pure machine side of job playback (no ESP-IDF, host-tested). On a Z1 the
// LPC holds no file: after the app's `play <path>` it asks the Mainboard for
// the file, line by line, over the UART (MakeraZ1Firmware Player.cpp and
// SerialConsole.cpp):
//
//   LPC PLAY_START {crc16 of path}         us PLAY_VIEW {crc16, u32 size}
//   LPC PLAY_DATA  {crc16, u32 line, u16 free}
//                                          us PLAY_DATA {crc16, u32 line, whole lines}
//                                             or PLAY_END once past the last line
//   LPC GOTO_START {crc16, u32 line N}     us GOTO_LINES {crc16, u32 lines, u32 bytes}
//   LPC PLAY_CAN   (abort), PLAY_END (done) us PLAY_CAN (read error, unknown job)
//
// A PLAY_START is served when it matches the file a client armed with
// `play` (for PLAY_ARM_TTL_MS), or the file played last: the
// machine's button restarts the last job (MainButton::on_idle,
// Player::on_set_public_data). Both live in RAM only, so after a reboot every
// PLAY_START is refused. Files are told apart by their CRC-16 alone, as stock
// does; two paths with the same CRC are not distinguished.
//
// Lines are 0-based on the wire. Empty lines count; a line longer than
// LPC_LINE_MAX is sent without its comments and blanks; if it is still too
// long the job is stopped (play_out_t.too_long). The core only ever answers
// the LPC, and only for the file a client armed, or (button restart) the last
// file played since boot.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mkframe.h"

// Data bytes per PLAY_DATA reply: the LPC accepts 510-byte payloads, minus
// crc16 and the line number.
#define PLAY_DATA_MAX (LPC_MAX_TX_PAYLOAD - 6)
// A goto scans this much of the file per GOTO_LINES update; each update
// restarts the LPC's 1 s goto timeout (Player::goto_command). 16 KB take about
// 0.2 s on a typical card.
#define PLAY_GOTO_STEP (16 * 1024)
// An armed `play` is matched only this long: the LPC sends PLAY_START 300 ms
// after the line (Player::play_command), or never (not homed, busy).
#define PLAY_ARM_TTL_MS 5000
// The reply buffer every call writes into.
#define PLAY_OUT_MAX (6 + PLAY_DATA_MAX)

// Reads n bytes at offset, fewer only if the file ends first; returns the
// count (0 at end of file) or -1.
typedef int (*play_read_fn)(void *ctx, uint32_t offset, uint8_t *buf, size_t n);

typedef enum { PLAY_IDLE, PLAY_OPENING, PLAY_PLAYING } play_state_t;
typedef enum { PLAY_FILE_NONE, PLAY_FILE_PENDING, PLAY_FILE_LAST } play_file_t;

typedef struct {
    bool has_pending, has_last;
    uint16_t pending_crc, last_crc;  // armed by `play`; played last
    uint32_t pending_ms;             // when it was armed
    play_file_t opening;
    play_state_t state;              // the job being served
    uint16_t crc;
    uint32_t size;
    uint32_t line, off;              // next line to serve and where it starts
    uint32_t chunk_line, chunk_off;  // the last reply, for a re-request
    bool seeking;                    // a goto scan is in progress
    uint32_t goto_target;
} play_sm_t;

typedef struct {
    uint8_t type;  // frame for the LPC, payload in out; 0: nothing to send
    size_t len;
    bool close;        // close the open file (done before any open)
    play_file_t open;  // open this file, then call play_opened/play_open_failed
    bool more;     // a goto is under way: call play_goto_step again
    bool too_long;     // with PLAY_CAN: line too_long_line has more than LPC_LINE_MAX bytes of code
    uint32_t too_long_line;
} play_out_t;

// The LPC restarted (its boot frames arrived): a job it was playing is over,
// and so is a pending `play`. The file played last stays, for the button.
// The caller closes the open file.
void play_lpc_reset(play_sm_t *p);

// A client sent `play <path>`; crc is the CRC-16 the LPC will
// compute over the name as it decodes it (xfer_path_lpc_crc). Never touches a
// job being served: the LPC refuses a second play anyway.
void play_arm(play_sm_t *p, uint16_t crc, uint32_t now_ms);

play_out_t play_on_lpc(play_sm_t *p, uint8_t type, const uint8_t *payload, size_t n, play_read_fn read, void *ctx,
                       uint8_t *out, uint32_t now_ms);
play_out_t play_opened(play_sm_t *p, uint32_t size, uint8_t *out);
play_out_t play_open_failed(play_sm_t *p);
play_out_t play_goto_step(play_sm_t *p, play_read_fn read, void *ctx, uint8_t *out);

// A job is being served (between PLAY_START and the LPC's PLAY_END/PLAY_CAN).
bool play_serving(const play_sm_t *p);
