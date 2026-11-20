#pragma once
// The "Makera" frame used on both links of the Mainboard: the UART to the
// LPC1768 (as implemented by the LPC in MakeraZ1Firmware:
// src/modules/communication/SerialConsole.cpp, src/libs/PublicData.h,
// src/libs/Kernel.h) and TCP 2222 to the official app (captured from a stock Z1).
//
//   0x86 0x68 | len_hi len_lo | type | payload[n] | crc_hi crc_lo | 0x55 0xAA
//   len = n + 3; CRC-16/XMODEM over the len, type and payload bytes.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MK_HEADER 0x8668
#define MK_FOOTER 0x55AA
#define MK_FRAME_OVERHEAD 9
#define MK_MAX_PAYLOAD (0xFFFF - 3)  // the u16 len field holds n + 3

// LPC link limits: the LPC rejects frames to it with more than 510 payload
// bytes (SerialConsole::on_serial_char_received) but sends up to 512
// (PacketMessage).
#define LPC_MAX_TX_PAYLOAD 510
#define LPC_MAX_RX_PAYLOAD 512
// The LPC bootloader's firmware packets (not the LPC firmware's limit above):
// FIRM_DATA carries a u32 frame number and 512 bytes, 516 in all (stock's log
// "type=0xC3, data_len=516").
#define LPC_FIRM_PACKET 512
#define LPC_FIRM_TX_PAYLOAD (4 + LPC_FIRM_PACKET)
// INFERRED: the longest job line the LPC can take. Its queue entries are 64
// bytes (Player.h, StaticQueue) and it copies each received line into char buf[130]
// without a bound (SerialConsole::on_serial_char_received), so the limit also protects the
// LPC. Stock replaces longer lines with an empty line ("Long line replaced
// with empty line" in its log).
#define LPC_LINE_MAX 63

// Mainboard -> LPC, and app -> Mainboard
#define PTYPE_CTRL_SINGLE 0xA1   // one realtime char: '?' status, '!' pause, '~' resume, 0x18 halt
#define PTYPE_CTRL_MULTI 0xA2    // a command line, e.g. G-code or "suspend"/"abort"/"resume"
#define PTYPE_FILE_START 0xB0
#define PTYPE_FILE_MD5 0xB1
#define PTYPE_FILE_VIEW 0xB2
#define PTYPE_FILE_DATA 0xB3
#define PTYPE_FILE_END 0xB4
#define PTYPE_FILE_CAN 0xB5
#define PTYPE_FILE_RETRY 0xB6
#define PTYPE_FILE_PLAYING 0xB7  // app: empty; Mainboard: "<path>|<md5>" of the job file
#define PTYPE_CONFIG_START 0xD1
#define PTYPE_CONFIG_VIEW 0xD2
#define PTYPE_CONFIG_DATA 0xD3
#define PTYPE_CONFIG_END 0xD4
#define PTYPE_CONFIG_CAN 0xD5
#define PTYPE_FACTORY_START 0xE1
#define PTYPE_FACTORY_VIEW 0xE2
#define PTYPE_FACTORY_DATA 0xE3
#define PTYPE_FACTORY_END 0xE4
#define PTYPE_FACTORY_CAN 0xE5
#define PTYPE_PLAY_START 0xF1
#define PTYPE_PLAY_VIEW 0xF2
#define PTYPE_PLAY_DATA 0xF3
#define PTYPE_PLAY_END 0xF4
#define PTYPE_PLAY_CAN 0xF5
#define PTYPE_GOTO_START 0xF6
#define PTYPE_GOTO_LINES 0xF7

// The LPC bootloader's firmware update (not in the GPL source; stock's log and
// strings, see components/lpcfw/include/lpcfw_sm.h). Both directions.
#define PTYPE_FIRM_START 0xC1  // the bootloader offers an update; our empty ack accepts it
#define PTYPE_FIRM_VIEW 0xC2   // the bootloader asks for the size; we answer u32 frames, u16 512
#define PTYPE_FIRM_DATA 0xC3   // the bootloader asks for u32 frame n; we answer n + its bytes
#define PTYPE_FIRM_END 0xC4    // the bootloader has all frames (INFERRED)
#define PTYPE_FIRM_CAN 0xC5    // from us: no update, the bootloader starts the LPC firmware

// LPC -> Mainboard, and Mainboard -> app
#define PTYPE_FIRM_VER 0x71
#define PTYPE_STATUS_RES 0x81
#define PTYPE_DIAG_RES 0x82
#define PTYPE_LOAD_INFO 0x83
#define PTYPE_LOAD_FINISH 0x84
#define PTYPE_LOAD_ERROR 0x85
#define PTYPE_NORMAL_INFO 0x90
#define PTYPE_CLIENT_ERROR 0x91  // stock: a refusal for one client (too many clients, transfer busy)

uint16_t mk_crc16(const uint8_t *data, size_t len);

// FIRM_* frames (0xC1-0xC5, the LPC bootloader's firmware download) carry the
// bootloader's CRC variant instead; mk_frame_encode and the parser pick it by type.
#define MK_TYPE_IS_FIRM(t) ((t) >= 0xC1 && (t) <= 0xC5)
uint16_t mk_crc16_lpcboot(const uint8_t *data, size_t len);

// Writes a frame into out (capacity out_cap). Returns frame length, or 0 if
// n exceeds MK_MAX_PAYLOAD or out is too small. Link-specific limits (such as
// LPC_MAX_TX_PAYLOAD) are the caller's to enforce.
size_t mk_frame_encode(uint8_t type, const void *payload, size_t n, uint8_t *out, size_t out_cap);

typedef void (*mk_frame_cb_t)(uint8_t type, const uint8_t *payload, size_t n, void *ctx);

typedef enum {
    // UART: bytes can be lost, so any bad frame is rescanned byte by byte for
    // the next header.
    MK_LOSSY,
    // TCP: nothing is lost or reordered, so a frame's length can be trusted.
    // An oversized frame, or one whose CRC fails behind a valid footer, is
    // skipped whole; a frame that happens to sit inside its payload is never
    // acted on. Only a wrong footer (the length itself is wrong) rescans.
    MK_RELIABLE,
} mk_stream_t;

typedef struct {
    uint8_t *buf;
    size_t cap;
    mk_stream_t mode;
    size_t have;
    size_t need;
    size_t skip;  // MK_RELIABLE: bytes of an oversized frame still to discard
    uint32_t bad_crc;
    uint32_t bad_frame;
} mk_parser_t;

// buf holds one frame; frames with a payload above cap - MK_FRAME_OVERHEAD
// are dropped (counted in bad_frame). cap must be at least MK_FRAME_OVERHEAD.
void mk_parser_init(mk_parser_t *p, uint8_t *buf, size_t cap, mk_stream_t mode);

// True while a frame is partly received (or an oversized one is being skipped).
static inline bool mk_parser_busy(const mk_parser_t *p)
{
    return p->have || p->skip;
}

// Gives up on a partly received frame, e.g. after a timeout, so that a stalled
// or bogus frame cannot hold back the frames behind it.
void mk_parser_drop_partial(mk_parser_t *p);

// Feeds received bytes; calls cb once per complete, valid frame. Bytes outside
// a valid frame are dropped and the parser resynchronises on the next header.
void mk_parser_feed(mk_parser_t *p, const uint8_t *data, size_t len, mk_frame_cb_t cb, void *ctx);
