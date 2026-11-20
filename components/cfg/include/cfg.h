#pragma once
// config.txt for the app's settings page and for the LPC, as the stock
// Mainboard does it (CONFIG_MANAGER and cfg_LPC1768 in its log and strings):
//   config-get [sd] <key>, config-set sd <key> <value>, config-restore
//   (config.default -> config.txt), config-default (config.txt ->
//   config.default), and the LPC's config load over PTYPE_CONFIG_* at its
//   boot. Both files are replaced atomically and written no other way.

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Runs one config command; writes the reply (stock's wording, ending in
// "\r\n") into out. args are the words after the command (NULL if absent).
void cfg_command(const char *cmd, const char *a1, const char *a2, const char *a3, char *out, size_t cap);

// Starts the task that serves the LPC's config load. Call before lpc_link.
esp_err_t cfg_start(void);

// Feed every PTYPE_CONFIG_* frame from the LPC; never blocks (the task
// replies over lpc_link).
void cfg_on_lpc_frame(uint8_t type, const uint8_t *payload, size_t n);

// The LPC's config load since this boot, one line without newline:
// "not requested since boot", "serving 98 lines since 1180 ms",
// "served 98 lines at 1250 ms", "cancelled at .. ms", "failed: <reason> at .. ms".
void cfg_lpc_status(char *out, size_t cap);
