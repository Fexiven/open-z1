#pragma once
// Pure handling of file paths sent by the official app.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XFER_PATH_MAX 200

// Decodes the app's escapes in file and Wi-Fi command arguments (0x01 space,
// 0x02 '?', 0x03 '&', 0x04 '!', 0x05 '~'; the Carvera Controller's mapping)
// and checks the result is a safe absolute path below /sd: no "." or ".."
// segments, no empty segments, no backslashes or other control bytes, and at
// most XFER_PATH_MAX bytes. Returns false if the path is refused.
bool xfer_path_decode(const char *in, size_t n, char *out, size_t cap);

// The CRC-16 the LPC computes over a `play` path: it decodes the app's escapes
// itself, with 0x03 as '*' where the app (and xfer_path_decode) mean '&'
// (shift_parameter in the GPL firmware's libs/utils.cpp). So a job named "a&b.nc" is opened as such, but armed with the CRC of
// "a*b.nc".
uint16_t xfer_path_lpc_crc(const char *in, size_t n);

// Whether a decoded path may be created, replaced, renamed or deleted. Refused:
// /sd and /sd/gcodes themselves, hidden names (a segment starting with '.',
// such as the sidecar folders), and the machine's own files: config.txt,
// config.default, factory.ini, firmware.bin (the app's firmware package: only
// its upload is allowed, by the caller) and lpc1768.bin (only the update
// writes it, from a checked package). Compared as FAT does (any case); segments FAT would alias (a trailing
// '.' or ' ', '~' and a digit as in 8.3 names) are refused.
bool xfer_path_writable(const char *path);

// The stock sidecar of a decoded path, kind ".md5" or ".lz" (layout of a stock
// card): files below /sd/gcodes keep it at /sd/gcodes/<kind>/<path below
// gcodes>, other files at /sd/<kind>/<name>. Returns false if it does not fit.
bool xfer_sidecar(const char *path, const char *kind, char *out, size_t cap);
