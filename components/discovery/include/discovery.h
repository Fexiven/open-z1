#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Announces the machine on the LAN the way the official app expects (see
// discovery_fmt.h). busy() reports whether an app is connected; the state
// comes from machine.h ("Offline" while the LPC is not reporting).
esp_err_t discovery_start(unsigned tcp_port, bool (*busy)(void));

// How often to call discovery_tick: a stock Z1 announces itself about every
// 500 ms (captured).
#define DISCOVERY_PERIOD_MS 500

// One announcement, if the station has an IP. Main's housekeeping calls it
// every DISCOVERY_PERIOD_MS.
void discovery_tick(void);
