#pragma once
// For installers outside this component (components/fwupdate: the GitHub
// download); POST /ota uses the same steps.

#include <stdbool.h>

#include "esp_app_desc.h"

// Starts an update: refused (returns why) while the flash.py migration is
// unfinished, the running image is unconfirmed, another update runs, or
// s_busy() says so. On NULL, ota_active() is true until ota_release().
const char *ota_claim(void);
void ota_release(void);

// The verified image new may replace the running one: same project name (it
// carries the board and the flash layout, see CMakeLists.txt) and, unless
// forced, not an older version. Returns why not (and sets *status), or NULL.
const char *ota_check_desc(const esp_app_desc_t *new, bool force, const char **status);

// Asked again right before the new image is selected; returns why not, or NULL.
const char *ota_busy_now(void);
