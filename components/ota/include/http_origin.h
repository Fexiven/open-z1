#pragma once
// Request checks for the port-80 server against other web pages in the
// same browser. Pure, host-tested.

#include <stdbool.h>

// True when origin (a request's Origin header) is the page served by host (its
// Host header): exactly "http://" + host, ASCII case-insensitive. "null"
// (sandboxed frames, file:// pages) and every other site are false.
bool http_origin_ok(const char *origin, const char *host);

// True when host (a Host header) names this board the way the LAN does: an
// IPv4 literal, or own_name (the DHCP hostname, "makera-z1-5566") with or
// without ".local", each optionally with ":port". Anything else is a DNS
// rebinding attempt: a foreign name made to resolve to the board, which would
// make the attacker's page "same origin".
bool http_host_ok(const char *host, const char *own_name);
