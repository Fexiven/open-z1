#pragma once
// The SD card, mounted at STORAGE_ROOT the way the official app addresses it
// ("/sd/config.txt", "/sd/gcodes/..."). Never formatted by this firmware; with
// CONFIG_Z1_SD_READ_ONLY nothing on it may be modified either.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define STORAGE_ROOT "/sd"

// Mounts the card (SDMMC slot 1, 4-bit). It is also mounted when inserted
// later and unmounted when pulled (card detect). An error: not mounted (not
// inserted, unreadable); the card is left as it is.
esp_err_t storage_start(void);

bool storage_mounted(void);

// Every open file or folder on the card holds it: storage_hold() before the
// open (false: no card, don't open), storage_release() after the close. A
// pulled card is unmounted only once the last hold is released.
bool storage_hold(void);
void storage_release(void);

// Whether modifying operations must be refused.
bool storage_read_only(void);

// Used and total space of the mounted card in MiB, refreshed every 5 s by the
// card monitor (never blocks); 0, 0 without a card.
void storage_space_mb(uint32_t *used, uint32_t *total);
