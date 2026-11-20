#pragma once
// App versions as in CONFIG_APP_PROJECT_VER: "MAJOR.MINOR.PATCH", optionally
// followed by "-<anything>" (ignored). Pure, host-tested.

#include <stdbool.h>

// Compares the numeric parts of a and b: <0, 0 or >0. A version that does not
// start with a number sorts below every numbered one.
int ota_version_cmp(const char *a, const char *b);
