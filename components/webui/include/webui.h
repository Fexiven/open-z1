#pragma once
// Web UI v1 on the OTA server's port 80 (no second server):
//
//   GET  /            one static page (www/index.html), polls /api/status every 1 s
//                     while visible; its graphs keep 5 minutes in the browser only
//   GET  /api/status  JSON (webui_fmt.h): machine, job, LPC, SD, Wi-Fi, system
//   GET  /api/log     the last ?n= (default 100, max 400) log lines, text/plain;
//                     ?since=<cursor>: only the lines after it. The (opaque) cursor for the
//                     next poll is in X-Log-Next; X-Log-Gap: 1 if lines were left
//                     out (lost, over 15 KB, or a restart). The page's log panel
//                     polls it every 2 s while open
//   GET  /api/diag    status, versions and the log as a .txt download
//   POST /api/stop    halt (0x18) to the LPC; not an emergency stop
//   POST /api/pause   feed hold ('!') to the LPC
//   GET  /api/snapshot one current camera JPEG (404 without a camera); the
//                     page's "Show camera" fetches one a second while on
//   POST /ota         the signed update (components/ota), also from the page
//
// LAN-only and unauthenticated, like the app's port 2222, so it offers only
// the safe direction (stop, pause) and the signed update: no resume, start,
// jog or spindle. POSTs to /api/ need an X-Requested-With header and, if
// Origin is sent, our own Origin (ota_same_origin): another web page open in
// the same browser cannot trigger them. No SSID, password, serial number or
// client address is served; the page never opens the camera's stream (port 82)
// and its snapshots take no viewer slot.
// GETs of /api/ and every POST need a Host naming the board (an IPv4 address
// or its hostname, against DNS rebinding).
//
// Clients of the setup hotspot (wifi_mgr.h) get only the Wi-Fi setup page:
//   GET  /             www/setup.html instead of the web UI
//   POST /setup/scan   starts a scan; POST /setup/join, body "<ssid>\n<password>",
//                      the app's `wlan` join (saved only once it gives an IP)
//   GET  /setup/result the outcome, polled by the page
// Everything else refuses them (ota_host_ok), and /setup/ refuses LAN clients.
//
// One httpd task serves all of port 80: while an update uploads, Stop and
// Pause over the web wait for it (the app's Stop on port 2222 does not).

#include <stdbool.h>

#include "esp_err.h"

// Call after ota_start. lpc_silent: whether stock's silent-LPC alarm is on.
esp_err_t webui_start(bool (*lpc_silent)(void));
