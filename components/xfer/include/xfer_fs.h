#pragma once
// File operations on the SD card for the app, keeping the stock sidecars
// (xfer_sidecar) in step. Paths are decoded and checked by the caller
// (xfer_path_decode, xfer_path_writable for every write).

#include <stdbool.h>
#include <stddef.h>

// Lists dir as xfer_ls_line lines; emit gets whole lines, at most cap bytes
// per call. Returns false if dir cannot be listed.
bool xfer_fs_ls(const char *dir, bool sizes, size_t cap, void (*emit)(void *ctx, const char *text, size_t n),
                void *ctx);

// A replacement cut short by a power loss, between xfer_fs_keep's moving the
// old file aside (.<name>.old) and putting the new one in place, leaves path
// missing: this puts the old file back. Called before path is read or
// replaced, so a later read never finds the file gone. Never deletes anything
// (a .<name>.part left behind is overwritten by the next upload of that name).
// Writes the card, so callers skip it when storage_read_only().
void xfer_fs_recover(const char *path);

// Deletes a file, or a folder with everything in it, and their sidecars.
bool xfer_fs_rm(const char *path);
// Renames a file or folder (not over an existing one), and its sidecars.
bool xfer_fs_mv(const char *from, const char *to);
bool xfer_fs_mkdir(const char *path);

// Puts a completely received upload in place under its real name: replaces
// any old file, records md5 in the .md5 sidecar (none if md5 is NULL), and
// drops stale sidecars.
bool xfer_fs_keep(const char *tmp, const char *path, const char *md5);
