#pragma once
// The log on the SD card, as stock: every ESP_LOG line, also mirrored to
// /sd/.logs/GeneralInfo.log with a "[time] " prefix, rotated to .log.1 and
// .log.2 at 3 MiB. The console output is unchanged.
//
// Lines go into a RAM ring first (never blocking the caller; a line that
// meets another one being taken is dropped) and a low-priority task appends
// them to the card every 2 s. Without a card, or with a read-only one, they
// stay in the ring only.
//
// Privacy: SSIDs, Wi-Fi passwords and the machine serial never reach the card
// or the ring. Passwords and the serial are never logged at all. The Wi-Fi
// driver's own lines (tags wifi, net80211, wpa*) stay on the console, and so
// does every line of ours that prints an SSID: wrap it in SDLOG_CONSOLE_ONLY.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SDLOG_DIR "/sd/.logs"
#define SDLOG_FILE SDLOG_DIR "/GeneralInfo.log"

// Installs the log hook; call first thing in app_main. The card is used once
// storage_start has mounted it.
esp_err_t sdlog_start(void);

// The last `lines` complete lines (as written to the card), NUL-terminated.
// Returns the length. next (may be NULL): the cursor for sdlog_since.
size_t sdlog_tail(char *out, size_t cap, unsigned lines, uint64_t *next);

// The complete lines logged since the cursor *pos, at most cap - 1 bytes,
// NUL-terminated; moves *pos past them. *gap: lines were left out (lost from
// the ring, more than fit, or a cursor from before a restart: then the tail
// that fits). Returns the length.
size_t sdlog_since(uint64_t *pos, char *out, size_t cap, bool *gap);

// Runs `stmt` (an ESP_LOGx call) with its output for the console only.
#define SDLOG_CONSOLE_ONLY(stmt)    \
    do {                            \
        sdlog_console_only(true);   \
        stmt;                       \
        sdlog_console_only(false);  \
    } while (0)

void sdlog_console_only(bool on);
