#pragma once
// Pure routing of frames from the official app (TCP 2222) to the LPC or to
// local handlers, as stock does.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CTRL_IGNORE,             // drop silently
    CTRL_FORWARD_REALTIME,   // pass the CTRL_SINGLE byte to the LPC
    CTRL_FORWARD_LINE,       // pass the (CR/LF-stripped) line to the LPC as CTRL_MULTI
    CTRL_LOCAL_VERSION,      // answer "version = ..." ourselves
    CTRL_LOCAL_M485,         // answer the protocol query; never forward (would switch the LPC's UART to text)
    CTRL_LOCAL_SN,           // `sn-get`: answered from the factory NVS
    CTRL_LOCAL_SYS_TIME,     // `sys-time`: first boot, power-on and machine seconds
    CTRL_LOCAL_TIME,         // `time [<epoch>]`: sets our clock (silently) or tells it; line_off/line_len
    CTRL_REJECT,             // answer with an error; reason in ctrl_route_t.reason
    CTRL_FILE_DOWNLOAD,      // FILE_START "download <path>": path at line_off/line_len (still escaped)
    CTRL_FILE_UPLOAD,        // FILE_START "upload <path>": the same
    CTRL_LOCAL_FTYPE,        // `ftype`: which upload format we take
    CTRL_LOCAL_LS,           // `ls [-e] [-s] <dir>`: args[0]; ls_sizes for -s
    CTRL_LOCAL_RM,           // `rm <path> [-e]`: args[0]
    CTRL_LOCAL_MV,           // `mv <from> <to> [-e]`: args[0], args[1]
    CTRL_LOCAL_MKDIR,        // `mkdir <path> [-e]`: args[0]
    CTRL_LOCAL_CONFIG,       // config-get/-set/-restore/-default: the word at line_off, args[0..2]
    CTRL_LOCAL_WLAN,         // `wlan -e` scan, `wlan <ssid> [<password>] -e` join, `wlan -d disconnect`
    CTRL_FILE_FRAME,         // another FILE_* frame: belongs to the client's transfer, if any
    CTRL_LOCAL_PLAYING,      // FILE_PLAYING (0xB7): which file the job plays, "<path>|<md5>"
    CTRL_LOCAL_UPGRADE,      // `upgrade`: the motion controller's update from /sd/firmware.bin (components/lpcfw)
    CTRL_LOCAL_UPGRADE_CANCEL,  // `upgrade cancel`: drop a waiting /sd/lpc1768.bin
    CTRL_LOCAL_RESET,        // `reset`: the same while /sd/firmware.bin exists (as stock), else forwarded
                             // (line_off/line_len) and the LPC restarts
} ctrl_action_t;

typedef struct {
    ctrl_action_t action;
    const char *reason;   // CTRL_REJECT only
    bool load_reply;      // the app waits for LOAD_FINISH/LOAD_ERROR (file and Wi-Fi commands)
    bool file_cancel;     // set for every FILE_START: a refusal must be FILE_CAN, since during a
                          // transfer the app ignores every frame type below FILE_MD5, LOAD_ERROR included
    size_t line_off;      // CTRL_FORWARD_LINE: the line without leading blanks starts here...
    size_t line_len;      // ...and is this long without trailing CR/LF
    size_t nargs;         // CTRL_LOCAL_LS/RM/MV/MKDIR/CONFIG: the arguments (for files, not the options)
    size_t arg_off[3];    // (still escaped)
    size_t arg_len[3];
    bool ls_sizes;
    bool wlan_eof;        // CTRL_LOCAL_WLAN: -e, reply with LOAD_INFO + LOAD_FINISH/LOAD_ERROR
    bool wlan_off;        // CTRL_LOCAL_WLAN: -d (disconnect)
    bool secret;          // a `wlan` line, whatever the action: may hold a password, wipe it after use
    bool track_play;      // CTRL_FORWARD_LINE of `play <path>`: arm the job before forwarding...
    size_t path_off;      // ...whose path (still escaped) starts here...
    size_t path_len;      // ...and is this long
} ctrl_route_t;

ctrl_route_t ctrl_route(uint8_t type, const uint8_t *payload, size_t n);

// Undoes the app's escapes in a command word (0x01 space, 0x02 '?', 0x03 '&',
// 0x04 '!', 0x05 '~') into out. False if it does not fit (n >= cap).
bool ctrl_unescape(const uint8_t *in, size_t n, char *out, size_t cap);

// Builds the reply to `version`: "version = <lpc>.<ours>\n". The app reads the
// first three components as the firmware version and treats a letter right
// after them as community firmware, so only the digits-and-dots prefix of
// ours is appended. lpc may be NULL when unknown ("0.0.0"). Returns the
// length, or -1 if it does not fit.
int ctrl_version_reply(char *out, size_t cap, const char *lpc, const char *ours);

// The stop bytes ('!' feed hold, 0x18 halt, 0x19 jog stop) of every complete,
// valid CTRL_SINGLE frame in buf, in order, into out (at most cap). Frames cut
// by the buffer's ends are not seen; the parser still handles them. Resume
// ('~') and the rest are never taken early.
size_t ctrl_stop_bytes(const uint8_t *buf, size_t n, uint8_t *out, size_t cap);

// How an `ls` ends: LOAD_FINISH "Load directory finished.\r\n" (stock's text)
// after a listing, and also for a folder that cannot be opened, as an empty
// listing (the app's timelapse tab lists /sd/videos, which a factory card does
// not have; stock only logs "Can't open dir", so INFERRED). LOAD_ERROR only
// for a path that is not allowed or no card (path_ok false). Sets *text.
uint8_t ctrl_ls_end(bool path_ok, const char **text);

// The LPC's DIAG_RES "{...}\n" with "|RSSI:<rssi>" before the "}", as stock's
// Mainboard sends it (the Z1 LPC has no Wi-Fi). Returns the
// length, or -1 if the reply does not end in "}\n" or does not fit.
int ctrl_diag_rssi(char *out, size_t cap, const uint8_t *in, size_t n, int rssi);

// The LPC's STATUS_RES "<...>\n" with stock's Mainboard fields before the ">":
// "|E:0,0,0,<sd used MB>,<sd total MB>|OTA:<phase>,<pct>" (stock's
// format "|E:%u,%u,%u,%lu,%lu|OTA:%u,%u>"; the first three E fields
// are INFERRED zeros; OTA is the update's phase and percentage, see lpcfw.h).
// Returns the length, or -1 if there is no closing '>' or it does not fit.
int ctrl_status_extras(char *out, size_t cap, const uint8_t *in, size_t n, uint32_t sd_used, uint32_t sd_total,
                       unsigned ota_phase, unsigned ota_pct);
