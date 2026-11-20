#pragma once
// Pure handling of config.txt (Smoothie format: "key value   # comment"
// lines, '#' lines are comments). Host-tested.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The longest line the LPC takes from the config transfer: it copies each
// line into char buf[130] (MakeraZ1Firmware FileConfigSource.cpp).
#define CFG_LINE_MAX 128

// Keys are words of [A-Za-z0-9._-]; values are one word without '#'.
bool cfg_key_ok(const char *key);
bool cfg_value_ok(const char *value);

// Copies key's value (the first line that sets it). False if it is not set.
bool cfg_get(const char *text, size_t n, const char *key, char *value, size_t cap);

// Writes text with key set to value into out: the value on key's first line
// is replaced and the rest of that line (spacing, comment) kept; a key that is
// not set is appended as stock's LPC does ("key         value         # added").
// Returns the new length, or -1 if it does not fit.
long cfg_set(const char *text, size_t n, const char *key, const char *value, char *out, size_t cap);

// The transfer to the LPC (CONFIG_VIEW/DATA): its lines are the non-empty
// '\n'-separated pieces of the file, numbered from 1; lines over CFG_LINE_MAX
// are left out, as the LPC could not take them.
uint32_t cfg_lpc_count(const char *text, size_t n);
// Packs whole lines from 1-based line `first` on into out (at most cap
// bytes), each ending in '\n', the last one too: the LPC reads one byte past
// the payload (the CRC), which an unterminated last line would swallow.
// Sets *lines to how many; 0 when there are none left.
size_t cfg_lpc_chunk(const char *text, size_t n, uint32_t first, char *out, size_t cap, uint32_t *lines);

// The 1-based file line of the first line too long for the LPC, or 0.
uint32_t cfg_lpc_long_line(const char *text, size_t n);
