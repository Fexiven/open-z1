#pragma once
// TCP 2222 server for the official app: framed protocol (see mkframe.h),
// commands routed per ctrl_route.h, LPC output relayed to every client.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// The official app's TCP port (stock).
#define CTRL_PORT 2222

esp_err_t ctrl_server_start(void);

// Feed every frame from the LPC. Status and console output go to all
// clients; FIRM_VER is kept for the `version` reply and not relayed.
// Safe to call from any task; never blocks on a client.
void ctrl_server_on_lpc_frame(uint8_t type, const uint8_t *payload, size_t n);

// Whether an app is connected (the "busy" flag of the LAN announcement).
bool ctrl_server_has_clients(void);

// A client is uploading a file.
bool ctrl_server_uploading(void);

// The app link has something in flight: a client's output queue is not empty,
// or a file transfer runs. The camera holds its frames back meanwhile. Cheap
// (one short lock); safe from any task.
bool ctrl_server_tx_busy(void);

// The LPC's firmware version from its FIRM_VER announcement ("1.1.3"), "" if
// none came yet.
void ctrl_server_lpc_version(char *out, size_t cap);

// Console `ctrl`: prints the LPC config/version state and, per connected
// client, its output queue, bad frames and whether a download runs.
void ctrl_server_dump(void);
