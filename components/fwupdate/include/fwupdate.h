#pragma once
// The web UI's firmware updates for both chips: the latest GitHub release,
// fetched by the board, and the motion controller's file upload
// (src/fwupdate.c has the endpoints).

#include "esp_err.h"

// Registers GET/POST /api/update and POST /api/lpcfw on the port-80 server.
// Call after ota_start and lpcfw_start.
esp_err_t fwupdate_start(void);
