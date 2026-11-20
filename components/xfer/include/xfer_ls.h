#pragma once
// Pure formatting of `ls` replies for the official app
// (stock format "%s%c %u %04d%02d%02d%02d%02d%02d").

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Whether a directory entry is listed: all but "." and "..", as stock (which
// lists .md5/ and .lz/).
bool xfer_ls_listed(const char *name);

// One line with sizes: "<dir>/ 0 <YYYYMMDDhhmmss>\r\n" or
// "<file>  <size> <YYYYMMDDhhmmss>\r\n" (two spaces), else "<name>[/]\r\n"
// (INFERRED; not captured). Spaces in name become 0x01. mtime is shown as stored (local time). Returns the length, or -1.
int xfer_ls_line(char *out, size_t cap, const char *name, bool dir, uint32_t size, int64_t mtime, bool sizes);
