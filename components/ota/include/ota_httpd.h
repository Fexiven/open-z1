#pragma once
// The OTA component's port-80 server, shared with the web UI (components/webui).
// Kept out of ota.h: esp_http_server.h brings in the socket API.

#include <stdbool.h>

#include "esp_http_server.h"

// The port-80 server, for the web UI's handlers (components/webui); NULL
// before ota_start or if it failed.
httpd_handle_t ota_httpd(void);

// The Host header names this board (http_host_ok: an IPv4 address or its own
// hostname), so a page on a rebound foreign name cannot read or post here,
// and the request did not come over the setup hotspot (wifi_mgr_on_setup_ap).
bool ota_host_ok(httpd_req_t *req);

// For every POST: ota_host_ok, and the Origin a browser sends with every
// cross-site POST is absent (curl) or this server's own page
// ("http://" + Host). POST /ota is refused otherwise (403), so another web
// page in the same browser cannot push an image.
bool ota_same_origin(httpd_req_t *req);
