#pragma once
// The RAM side of the SD log, pure and host-tested: a byte ring of complete
// log lines, and the rule that keeps the Wi-Fi driver's lines off the card.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bytes are numbered from 0 since the start; the ring keeps the last `size`.
typedef struct {
    char *buf;
    size_t size;
    uint64_t head;  // bytes ever written
} sdlog_ring_t;

void sdlog_ring_init(sdlog_ring_t *r, char *buf, size_t size);

// Appends n bytes, overwriting the oldest.
void sdlog_ring_put(sdlog_ring_t *r, const char *s, size_t n);

// Copies up to cap bytes from *pos on (two memcpy at most) and advances *pos. If the bytes at
// *pos were overwritten, it first moves on to the oldest line still whole
// and sets *lost. Returns the number of bytes copied.
size_t sdlog_ring_read(const sdlog_ring_t *r, uint64_t *pos, char *out, size_t cap, bool *lost);

// Keeps the last `lines` lines of buf[0..n) (moved to the front), dropping
// the first line too when cut_start (it may be partial). Returns the length.
size_t sdlog_last_lines(char *buf, size_t n, unsigned lines, bool cut_start);

// The length of buf[0..n) up to and including its last '\n' (0 without one).
size_t sdlog_whole_lines(const char *buf, size_t n);

// Whether an ESP-IDF log line ("I (123) tag: ...", colour codes allowed)
// comes from the Wi-Fi driver or supplicant (tags wifi, net80211, wpa*):
// those print SSIDs, so they stay on the console only.
bool sdlog_line_from_wifi_driver(const char *line, size_t n);
