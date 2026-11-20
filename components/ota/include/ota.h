#pragma once
// Push OTA over HTTP (POST /ota) and the first-boot check of a new image.
//
//   GET  /ota   "<project name> <version> <ELF sha256, first 9 hex> <slot> <valid|pending>\n"
//               "lpc_config=<cfg_lpc_status>\n" (the LPC's config load since boot)
//   POST /ota   body = a signed app image (build/z1/z1fw-z1-l3.bin);
//               ?force=1 allows an older version
//
// The image goes into the other OTA slot. esp_ota_end() checks its signature
// against the key that signed the running app (signed app verification
// without hardware secure boot: no eFuses involved), so only images signed
// with the same key install; then its project name must match the running one
// (it names the board and the flash layout), and its version must not be lower
// unless forced. The downgrade check prevents mistakes, not attacks: anyone on
// the LAN can add ?force=1.

#include <stdbool.h>

#include "esp_err.h"

// A new image must reach Wi-Fi and the control server within this, or the
// board restarts and the bootloader goes back to the previous image.
#define OTA_CONFIRM_TIMEOUT_S 120

// Returns why an update must wait (a job, an upload), or NULL.
typedef const char *(*ota_busy_fn)(void);

// Starts the HTTP server on port 80. A POST is refused while the
// running image is still unconfirmed.
esp_err_t ota_start(ota_busy_fn busy);

// An update is being received or installed: no new jobs, uploads or file writes.
bool ota_active(void);

// Call once at the end of startup, after ota_start. If this is the first boot
// of a new image, marks it valid as soon as healthy() returns true; if that does
// not happen within OTA_CONFIRM_TIMEOUT_S, restarts so the bootloader
// rolls back to the previous image. The timeout does not run while hold()
// (a job) is true. Otherwise does nothing.
void ota_confirm(bool (*healthy)(void), bool (*hold)(void));
