#pragma once
// The camera resolutions on offer (GET /api/status, POST /api/camera). Pure.

#include <stddef.h>

#define CAMERA_RES_COUNT 3
#define CAMERA_RES_DEFAULT 0  // 640x480: what the official app is built for, as stock

// "640x480", "1280x720", "1600x1200", by index.
extern const char *const camera_res_names[CAMERA_RES_COUNT];

// The index of the name in s[0..len), exactly one of the above; -1 otherwise.
int camera_res_parse(const char *s, size_t len);
