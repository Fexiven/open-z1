#pragma once
// Pure formatting of the LAN discovery datagram the official app listens for.
//
// Observed from a stock Z1 (UDP to port 3333, twice every ~500 ms):
//   "Makera_Z1_EEFF,192.0.2.21,2222,0,Idle"
//   name,ip,tcp_port,busy,state
// The Carvera Controller reads at most 128 bytes and splits on ',', so no
// field may contain a comma.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISCOVERY_MAX_LEN 128

// Writes the datagram (not NUL-counted) into out. Returns its length, or -1 if
// a field contains ',' or the result would not fit DISCOVERY_MAX_LEN / cap.
int discovery_format(char *out, size_t cap, const char *name, const char *ip, unsigned tcp_port, bool busy,
                     const char *state);

// "Makera_Z1_" plus the last two bytes of the Wi-Fi STA MAC in uppercase hex,
// as the stock firmware names the machine (and its softAP).
void discovery_name(char *out, size_t cap, const uint8_t mac[6]);
