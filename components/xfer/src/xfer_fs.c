#include "xfer_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xfer_ls.h"
#include "xfer_path.h"

#define PATH_CAP (XFER_PATH_MAX + 32)  // room for ".<name>.old" and the like

bool xfer_fs_ls(const char *dir, bool sizes, size_t cap, void (*emit)(void *ctx, const char *text, size_t n),
                void *ctx)
{
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    char chunk[1024], path[PATH_CAP], line[300];
    size_t len = 0;
    cap = cap < sizeof(chunk) ? cap : sizeof(chunk);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (!xfer_ls_listed(e->d_name) || snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path) ||
            stat(path, &st) != 0) {
            continue;
        }
        int n = xfer_ls_line(line, sizeof(line), e->d_name, S_ISDIR(st.st_mode), (uint32_t)st.st_size,
                             (int64_t)st.st_mtime, sizes);
        if (n < 0) {
            continue;
        }
        if (len + (size_t)n > cap) {  // lines are never split across frames
            emit(ctx, chunk, len);
            len = 0;
        }
        memcpy(chunk + len, line, (size_t)n);
        len += (size_t)n;
    }
    closedir(d);
    if (len) {
        emit(ctx, chunk, len);
    }
    return true;
}

// Deletes path, recursing into folders. path is a PATH_CAP buffer it may
// extend while it works.
static bool rm_tree(char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0;
    }
    if (depth > 16) {
        return false;
    }
    DIR *d = opendir(path);
    if (!d) {
        return false;
    }
    bool ok = true;
    size_t base = strlen(path);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        if (snprintf(path + base, PATH_CAP - base, "/%s", e->d_name) >= (int)(PATH_CAP - base)) {
            ok = false;
            continue;
        }
        ok &= rm_tree(path, depth + 1);
        path[base] = '\0';
    }
    closedir(d);
    return ok && rmdir(path) == 0;
}

static void rm_sidecars(const char *path)
{
    static const char *const kinds[] = {".md5", ".lz"};
    for (int i = 0; i < 2; i++) {
        char side[PATH_CAP];
        if (xfer_sidecar(path, kinds[i], side, sizeof(side))) {
            rm_tree(side, 0);  // usually absent
        }
    }
}

// Creates the folders leading to path, as needed.
static void mkdir_parents(const char *path)
{
    char p[PATH_CAP];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = strchr(p + 4, '/'); s; s = strchr(s + 1, '/')) {
        *s = '\0';
        mkdir(p, 0775);
        *s = '/';
    }
}

bool xfer_fs_rm(const char *path)
{
    char p[PATH_CAP];
    snprintf(p, sizeof(p), "%s", path);
    if (!rm_tree(p, 0)) {
        return false;
    }
    rm_sidecars(path);
    return true;
}

bool xfer_fs_mv(const char *from, const char *to)
{
    struct stat st;
    if (stat(to, &st) == 0 || rename(from, to) != 0) {
        return false;
    }
    static const char *const kinds[] = {".md5", ".lz"};
    for (int i = 0; i < 2; i++) {
        char a[PATH_CAP], b[PATH_CAP];
        if (xfer_sidecar(from, kinds[i], a, sizeof(a)) && xfer_sidecar(to, kinds[i], b, sizeof(b)) &&
            stat(a, &st) == 0) {
            mkdir_parents(b);
            rm_tree(b, 0);
            rename(a, b);
        }
    }
    return true;
}

bool xfer_fs_mkdir(const char *path)
{
    return mkdir(path, 0775) == 0;
}

static bool old_name(const char *path, char *old, size_t cap)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return false;
    }
    int n = snprintf(old, cap, "%.*s/.%s.old", (int)(slash - path), path, slash + 1);
    return n > 0 && (size_t)n < cap;
}

void xfer_fs_recover(const char *path)
{
    char old[PATH_CAP];
    struct stat st;
    if (stat(path, &st) != 0 && old_name(path, old, sizeof(old)) && stat(old, &st) == 0 && S_ISREG(st.st_mode)) {
        rename(old, path);
    }
}

bool xfer_fs_keep(const char *tmp, const char *path, const char *md5)
{
    xfer_fs_recover(path);
    // An old file is moved aside first and only deleted once the new one is
    // in place, so a failed rename never loses it.
    char old[PATH_CAP];
    if (!old_name(path, old, sizeof(old))) {
        return false;
    }
    struct stat st;
    bool had_old = stat(path, &st) == 0;
    if (had_old) {
        // A stale .old from an earlier power loss goes; without the real file
        // it would be the only copy, so it is kept then.
        unlink(old);
    }
    if (had_old && (S_ISDIR(st.st_mode) || rename(path, old) != 0)) {
        return false;
    }
    if (rename(tmp, path) != 0) {
        if (had_old) {
            rename(old, path);
        }
        return false;
    }
    if (had_old) {
        unlink(old);
    }
    rm_sidecars(path);
    char side[PATH_CAP];
    if (md5 && xfer_sidecar(path, ".md5", side, sizeof(side))) {
        mkdir_parents(side);
        FILE *f = fopen(side, "wb");
        if (f) {
            fwrite(md5, 1, 32, f);  // 32 hex digits, no newline, as on a stock card
            fclose(f);
        }
    }
    return true;
}
