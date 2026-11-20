#pragma once

#include "esp_err.h"

// Line console on the USB-Serial/JTAG port: `help`, `wifi status`, `wifi scan`,
// `wifi set "<ssid>" ["<password>"]`, `sd [dir]` (read-only listing), `ctrl`
// (server state). Input is not echoed, so passwords never
// appear in the serial log.
esp_err_t cli_start(void);
