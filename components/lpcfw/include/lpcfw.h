#pragma once
// The motion controller's (LPC1768) firmware update, as the official app's
// Firmware Update page drives it on stock: the app uploads Makera's bundle to
// /sd/firmware.bin and sends `upgrade` (older apps `reset`). A bare LPC1768
// image there works too (the web UI's motion controller update). Only the bundle's
// LPC1768 image is ever used; its ESP32 image is never installed (this
// firmware updates itself over its own OTA only).
//
//  1. `upgrade`: refused during a job, our own OTA, a transfer to the LPC, or
//     unless the machine reports Idle. The bundle is checked without trusting
//     it (lpcfw_bundle.h); its LPC image goes to /sd/lpc1768.bin (written
//     under a temporary name, read back, then renamed); /sd/firmware.bin is
//     deleted, whatever the outcome.
//  2. `reset\n` to the LPC (as stock): it restarts after 3 s, and its
//     bootloader asks for an update (lpcfw_sm.h), served from /sd/lpc1768.bin.
//     While it runs, nothing else reaches the LPC's console (lpc_link), and
//     the app gets a status with |OTA:2,<pct> (stock's phase and percentage).
//  3. Once the new LPC firmware runs, /sd/lpc1768.bin is deleted: |OTA:4,100
//     for 3 s, then |OTA:0,0. A failure shows |OTA:3,<pct> for 10 s and a text.
//
// The LPC is reset only on a client's command (`upgrade`, `reset`), never by
// this firmware on its own: a Z1 homes after every LPC restart. Clients get
// "The motion controller restarts and the machine will home." first.
//
// /sd/lpc1768.bin is the only state, served from RAM right in the LPC's
// receive task whenever the bootloader asks by itself (a power cycle, a
// bootloader left half-way). If the LPC firmware runs again before the
// bootloader took more than the first frame, nothing was replaced and the
// image is removed: no endless retries; `upgrade` again to retry. Otherwise it
// stays for recovery. `upgrade cancel` removes it (not mid-transfer).
// Logged at INFO (so in the SD log): the start, progress every 10 %, the end
// and errors; every FIRM_* frame in both directions only at DEBUG.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define LPCFW_BUNDLE "/sd/firmware.bin"
#define LPCFW_IMAGE "/sd/lpc1768.bin"

// Call after storage_start and before lpc_link_start.
esp_err_t lpcfw_start(void);

// Where results and errors go (every app client, as stock's texts).
void lpcfw_set_notify(void (*notify)(const char *text));

// Feed every frame from the LPC. Never blocks (FIRM_START without an image is
// declined right here; the rest is handled on lpcfw's task).
void lpcfw_on_lpc_frame(uint8_t type, const uint8_t *payload, size_t n);

// `upgrade` (and `reset` while a bundle is on the card). NULL if accepted
// (results follow through notify), else the refusal text for the client.
const char *lpcfw_upgrade(void);

// Whether lpcfw_upgrade would be accepted now (no update running, no job, no
// OTA, the machine Idle): NULL, else the refusal text. For callers that first
// put a file in place.
const char *lpcfw_can_upgrade(void);

// `upgrade cancel`: removes /sd/lpc1768.bin unless it is being transferred.
// NULL if accepted (results through notify), else the refusal.
const char *lpcfw_cancel(void);

// The text clients get before any LPC reset (also for a forwarded `reset`).
const char *lpcfw_reset_warning(void);

// Whether /sd/firmware.bin exists (for `reset`).
bool lpcfw_bundle_present(void);

// Checking a bundle, waiting for the LPC's bootloader, or serving it: no OTA,
// no jobs, no bundle upload meanwhile.
bool lpcfw_busy(void);

// Stock's |OTA:<phase>,<pct> status fields: 0 idle, 2 LPC transfer,
// 3 failed, 4 done (1, the ESP32 image, never happens here).
void lpcfw_ota_report(unsigned *phase, unsigned *pct);

// While the LPC is in its bootloader it sends no status, but the app follows
// the update in the status (stock then sends one of its own): fills out with
// the LPC's last status report (or a minimal Idle one) and returns true while
// a transfer is being reported and the LPC's own status is stale. cap:
// MACHINE_REPORT_MAX + 8 holds any.
bool lpcfw_status_stand_in(char *out, size_t cap);
