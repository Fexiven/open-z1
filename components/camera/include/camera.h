#pragma once
// Live camera for the official app: WebSocket ws://<ip>:82/ws_video. After the
// app's text "start_stream" every binary message is one JPEG (640x480). The
// sensor runs from boot, as on stock; frames are sent only while an app session
// is open and someone watches. One viewer at a time, as stock: a new viewer
// replaces the old one, which gets "The video stream channel is already
// occupied." and is closed.

#include <stdbool.h>

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// session_active: whether an app is connected (TCP 2222); no frames without
// one, as on stock. Returns ESP_ERR_NOT_SUPPORTED when the board has no camera
// pins configured or the build has no PSRAM.
esp_err_t camera_start(bool (*session_active)(void));

// One current JPEG for the web UI (GET /api/snapshot), without a viewer slot
// or an app session: a copy of the frame the viewer was just sent while a
// stream runs (the stream loses nothing), else of the next frame. Blocks at
// most about 300 ms. The caller frees *jpeg. ESP_ERR_NOT_SUPPORTED: no camera
// running; ESP_ERR_INVALID_STATE: another snapshot in flight; ESP_ERR_TIMEOUT,
// ESP_ERR_NO_MEM.
esp_err_t camera_snapshot(uint8_t **jpeg, size_t *len);
