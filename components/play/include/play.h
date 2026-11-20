#pragma once
// Job playback: serves the armed file to the LPC as it asks for it (see
// play_sm.h). Nothing is stored: after a reboot no job is armed.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t play_start(void);

// A client sent `play <path>`: path as decoded for opening, crc
// as the LPC computes it over its own decoding (xfer_path_lpc_crc).
void play_job_arm(const char *path, uint16_t crc);

// The LPC restarted (main.c sees its boot frames): ends a job it was playing,
// so the card, writes and OTA are free again. Never blocks.
void play_job_lpc_reset(void);

// Where play reports a job it had to stop (a line too long for the LPC);
// main relays it to every app client.
void play_set_notify(void (*notify)(const char *text));

// Feed every PLAY_* / GOTO_* frame from the LPC. Never blocks.
void play_job_on_lpc_frame(uint8_t type, const uint8_t *payload, size_t n);

// A job is being served (files must not change under it).
bool play_job_serving(void);

// The file of the job being served, or of the last one since boot; "" if none.
void play_job_file(char *out, size_t cap);

// A job is being served, or `play` was sent and the LPC may still start it.
bool play_job_busy(void);

// A job is being served and the machine reports Run (for the machine-seconds counter).
bool play_job_running(void);
