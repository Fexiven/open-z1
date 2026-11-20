// File transfers and file commands for the app (TCP 2222): downloads,
// uploads, ls, rm, mv, mkdir and FILE_PLAYING, one transfer at a time across
// all clients, as stock. Runs in the server task, except the file worker
// (rm, and the MD5 of a download without a sidecar).

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "ctrl_priv.h"
#include "ctrl_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lpcfw.h"
#include "ota.h"
#include "play.h"
#include "storage.h"
#include "xfer_fs.h"

static const char *TAG = "ctrl";

#define XFER_IDLE_US (10 * 1000 * 1000)
// Without a .md5 sidecar the MD5 is computed by the file worker; only for
// files this small, larger ones get XFER_DL_MD5_UNKNOWN, as on stock.
#define XFER_HASH_MAX (256 * 1024)
#define LS_CHUNK 1024  // whole lines per LOAD_INFO frame

static uint8_t s_xfer_out[FRAME_PAYLOAD_MAX];  // server task only

// Uploads in progress, for ota's busy check. Counted before ota_active() is
// read; ota sets its flag before reading this, so one of them always sees the other.
static atomic_int s_uploads;

bool ctrl_server_uploading(void)
{
    return atomic_load(&s_uploads) > 0;
}

// ---- the file worker: rm (a folder of 500 files and their sidecars takes
// tens of seconds) and the MD5 of a download, off the server task, which
// must keep relaying status and stop bytes. One job at a time.

typedef enum { JOB_RM, JOB_HASH } job_kind_t;

typedef struct {
    job_kind_t kind;
    int slot;
    uint32_t order;  // the client's connection, so a reply never reaches a newcomer
    char path[XFER_PATH_MAX + 1];
    bool ok;         // JOB_HASH: the result, read by the server task
    char md5[33];
} job_t;

// Filled by the server task while !s_job_busy, then the worker's until it
// sets s_hash_done (JOB_HASH; the server task frees it) or clears s_job_busy.
static job_t s_job;
static volatile bool s_job_busy, s_rm_busy, s_hash_done;
static TaskHandle_t s_job_task;

static void md5_hex(md5_context_t *ctx, char md5[33])
{
    uint8_t digest[ESP_ROM_MD5_DIGEST_LEN];
    esp_rom_md5_final(digest, ctx);
    for (int i = 0; i < ESP_ROM_MD5_DIGEST_LEN; i++) {
        sprintf(md5 + 2 * i, "%02x", digest[i]);
    }
}

// The worker's own handle: the server task may close its own at any time.
static bool hash_path(const char *path, char md5[33])
{
    if (!storage_hold()) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    bool ok = f != NULL;
    if (f) {
        md5_context_t ctx;
        uint8_t buf[512];
        esp_rom_md5_init(&ctx);
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
            esp_rom_md5_update(&ctx, buf, got);
        }
        ok = !ferror(f);
        fclose(f);
        if (ok) {
            md5_hex(&ctx, md5);
        }
    }
    storage_release();
    return ok;
}

static void job_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        job_t *j = &s_job;
        if (j->kind == JOB_HASH) {
            j->ok = hash_path(j->path, j->md5);
            ctrl_lock();  // publishes ok and md5 with the flag
            s_hash_done = true;  // the server task picks it up (ctrl_files_poll)
            ctrl_wake();
            ctrl_unlock();
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        bool ok = storage_hold();
        if (ok) {
            ok = xfer_fs_rm(j->path);
            storage_release();
        }
        ESP_LOGI(TAG, "rm %s: %s in %lld ms", j->path, ok ? "done" : "failed", (esp_timer_get_time() - t0) / 1000);
        ctrl_lock();
        if (ctrl_client_is(j->slot, j->order)) {
            client_t *c = &ctrl_clients[j->slot];
            if (!ok) {
                char text[XFER_PATH_MAX + 96];
                snprintf(text, sizeof(text), "Could not delete %s \r\n", j->path);  // stock's text
                ctrl_queue_frame(c, PTYPE_NORMAL_INFO, text, strlen(text));
            }
            ctrl_queue_frame(c, ok ? PTYPE_LOAD_FINISH : PTYPE_LOAD_ERROR, ok ? "ok\r\n" : "", ok ? 4 : 0);
        }
        ctrl_unlock();
        s_rm_busy = false;
        s_job_busy = false;
    }
}

esp_err_t ctrl_files_start(void)
{
    return xTaskCreate(job_task, "ctrl_job", 6144, NULL, 3, &s_job_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool run_job(client_t *c, job_kind_t kind, const char *path)
{
    if (s_job_busy) {
        return false;
    }
    s_job = (job_t){.kind = kind, .slot = (int)(c - ctrl_clients), .order = c->order};
    snprintf(s_job.path, sizeof(s_job.path), "%s", path);
    s_job_busy = true;
    s_rm_busy = kind == JOB_RM;
    xTaskNotifyGive(s_job_task);
    return true;
}

// ---- transfers

void ctrl_files_close(client_t *c)
{
    if (c->xfile) {
        fclose(c->xfile);
        c->xfile = NULL;
    }
    if (c->ul_tmp[0]) {
        remove(c->ul_tmp);
        c->ul_tmp[0] = '\0';
    }
    if (c->held) {
        c->held = false;
        storage_release();
    }
    if (c->uploading) {
        c->uploading = false;
        atomic_fetch_sub(&s_uploads, 1);
    }
    c->hashing = false;  // a result that still comes is dropped
    c->dl.state = XFER_DL_IDLE;
    c->ul.state = XFER_UL_IDLE;
}

static bool xfer_active(const client_t *c)
{
    return c->hashing || c->dl.state != XFER_DL_IDLE || c->ul.state != XFER_UL_IDLE;
}

// One file transfer at a time across all clients, as stock: an upload, a
// download or an rm.
static bool transfer_busy(const client_t *self)
{
    for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS; i++) {
        const client_t *o = &ctrl_clients[i];
        if (o != self && o->fd >= 0 && xfer_active(o)) {
            return true;
        }
    }
    return s_rm_busy;
}

static int xfer_read(void *ctx, uint32_t offset, uint8_t *buf, size_t n)
{
    FILE *f = ctx;
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        return -1;
    }
    size_t got = fread(buf, 1, n, f);
    return ferror(f) ? -1 : (int)got;
}

static void queue_out(client_t *c, uint8_t type, size_t len)
{
    if (type) {
        ctrl_lock();
        ctrl_queue_frame(c, type, s_xfer_out, len);
        ctrl_unlock();
    }
}

// The MD5 the stock layout keeps for each file (xfer_sidecar).
static bool read_sidecar_md5(const char *path, char md5[33])
{
    char side[XFER_PATH_MAX + 16];
    if (!xfer_sidecar(path, ".md5", side, sizeof(side))) {
        return false;
    }
    FILE *f = fopen(side, "rb");
    if (!f) {
        return false;
    }
    size_t got = fread(md5, 1, 32, f);
    fclose(f);
    md5[32] = '\0';
    if (got != 32) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        if (!((md5[i] >= '0' && md5[i] <= '9') || (md5[i] >= 'a' && md5[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

// The download is open (c->xfile) and its MD5 known: FILE_MD5 to the app.
static void begin_download(client_t *c, const char *md5, const char *source)
{
    c->xfer_last_us = esp_timer_get_time();
    xfer_out_t o = xfer_dl_begin(&c->dl, c->dl_size, md5, XFER_PACKET, s_xfer_out, sizeof(s_xfer_out));
    if (!o.done) {
        ESP_LOGI(TAG, "%s: download %s (%lu bytes, md5 %s from %s)", c->ip, c->xfer_path, (unsigned long)c->dl_size,
                 md5, source);
    }
    queue_out(c, o.type, o.len);
    if (o.done) {
        ctrl_files_close(c);
    }
}

void ctrl_files_download(client_t *c, const uint8_t *arg, size_t len)
{
    ctrl_files_close(c);
    if (transfer_busy(c)) {
        ctrl_busy_reply(c, TRANSFER_BUSY);
        return;
    }
    char path[XFER_PATH_MAX + 1];
    if (!storage_mounted()) {
        ctrl_reject(c, "no SD card", false, true);
        return;
    }
    if (!xfer_path_decode((const char *)arg, len, path, sizeof(path))) {
        ctrl_reject(c, "invalid path", false, true);
        return;
    }
    if (!storage_hold()) {
        ctrl_reject(c, "no SD card", false, true);
        return;
    }
    c->held = true;  // released by ctrl_files_close, on every way out
    if (!storage_read_only()) {
        xfer_fs_recover(path);  // a replacement cut short by a power loss
    }
    FILE *f = fopen(path, "rb");
    struct stat st;
    if (f && fstat(fileno(f), &st) == 0 && (st.st_size < 0 || (uint64_t)st.st_size > INT32_MAX)) {
        fclose(f);
        ctrl_files_close(c);
        ctrl_reject(c, "file too large", false, true);  // the offsets are 32-bit
        return;
    }
    if (!f || fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        if (f) {
            fclose(f);
        }
        ctrl_files_close(c);
        ESP_LOGI(TAG, "%s: download %s: not found", c->ip, path);
        ctrl_reject(c, "file not found", false, true);
        return;
    }
    c->xfile = f;
    c->dl_size = (uint32_t)st.st_size;
    snprintf(c->xfer_path, sizeof(c->xfer_path), "%s", path);
    char md5[33];
    // config.txt changes through config-set; a stock sidecar for it may be stale.
    if (strcasecmp(path, "/sd/config.txt") && read_sidecar_md5(path, md5)) {
        begin_download(c, md5, "sidecar");
    } else if (st.st_size > XFER_HASH_MAX) {
        begin_download(c, XFER_DL_MD5_UNKNOWN, "nowhere: too large to hash here");
    } else {
        // The worker is free: rm holds it only while transfer_busy() says so.
        c->hashing = run_job(c, JOB_HASH, path);
        c->xfer_last_us = esp_timer_get_time();
        if (!c->hashing) {
            begin_download(c, XFER_DL_MD5_UNKNOWN, "nowhere: the file worker is busy");
        }
    }
}

void ctrl_files_poll(int64_t now)
{
    ctrl_lock();
    bool done = s_hash_done;
    s_hash_done = false;
    bool same = done && ctrl_client_is(s_job.slot, s_job.order);
    ctrl_unlock();
    if (done) {
        job_t *j = &s_job;
        client_t *c = &ctrl_clients[j->slot];
        if (same && c->hashing) {
            c->hashing = false;
            if (j->ok) {
                begin_download(c, j->md5, "content");
            } else {
                ctrl_files_close(c);
                ESP_LOGW(TAG, "%s: download %s: read error", c->ip, j->path);
                ctrl_reject(c, "read error", false, true);
            }
        }
        s_job_busy = false;
    }
    for (int i = 0; i < CONFIG_Z1_CTRL_MAX_CLIENTS; i++) {
        client_t *c = &ctrl_clients[i];
        if (c->fd >= 0 && xfer_active(c) && now - c->xfer_last_us > XFER_IDLE_US) {
            ESP_LOGW(TAG, "%s: transfer abandoned by the app", c->ip);
            ctrl_files_close(c);
        }
    }
}

// Refuses a write the app asked for: the card, the path or the moment is wrong.
// bundle: the path may be the firmware package (an upload only).
static const char *write_refusal(const char *path, bool bundle)
{
    if (!storage_mounted()) {
        return "no SD card";
    }
    if (storage_read_only()) {
        return "the SD card is read-only in this firmware";
    }
    if (ota_active()) {
        return "busy updating";
    }
    if (bundle && !strcasecmp(path, LPCFW_BUNDLE)) {
        // The app's Firmware Update uploads here, for `upgrade` (lpcfw).
        if (lpcfw_busy()) {
            return "busy updating";
        }
    } else if (!xfer_path_writable(path)) {
        return "The path is not allowed.";  // stock's text
    }
    if (play_job_serving()) {
        return "a job is running";  // INFERRED: files must not change under a job
    }
    if (s_rm_busy) {
        return TRANSFER_BUSY;  // one write at a time while a folder goes away
    }
    return NULL;
}

// FILE_PLAYING: "<path>|<md5>" of the job file, as stock (the
// app asks after play, resume and a reconnect). The MD5 only from the
// sidecar, since hashing a big job here would stall the server; without one
// "<path>|" (stock has that format too). No job since boot: "|" (INFERRED).
void ctrl_files_playing(client_t *c)
{
    char path[XFER_PATH_MAX + 1], md5[33] = "";
    play_job_file(path, sizeof(path));
    if (path[0] && storage_hold()) {  // every open file holds the card
        if (!read_sidecar_md5(path, md5)) {
            md5[0] = '\0';
        }
        storage_release();
    }
    char text[XFER_PATH_MAX + 40];
    snprintf(text, sizeof(text), "%s|%s", path, md5);
    ctrl_reply_text(c, PTYPE_FILE_PLAYING, text);
}

// ---- file commands (ls, rm, mv, mkdir)

static void ls_emit(void *ctx, const char *text, size_t n)
{
    ctrl_queue_big(ctx, PTYPE_LOAD_INFO, text, n);
}

static bool arg_path(const uint8_t *payload, const ctrl_route_t *r, size_t i, char *out, size_t cap)
{
    return xfer_path_decode((const char *)payload + r->arg_off[i], r->arg_len[i], out, cap);
}

void ctrl_files_ls(client_t *c, const uint8_t *payload, const ctrl_route_t *r)
{
    char dir[XFER_PATH_MAX + 1];
    const char *arg = (const char *)payload + r->arg_off[0];
    bool root = (r->arg_len[0] == 3 && !memcmp(arg, "/sd", 3)) || (r->arg_len[0] == 4 && !memcmp(arg, "/sd/", 4));
    if (root) {
        strcpy(dir, STORAGE_ROOT);  // the card itself, which xfer_path_decode does not name
    }
    const char *text;
    if ((!root && !arg_path(payload, r, 0, dir, sizeof(dir))) || !storage_hold()) {
        ctrl_reply_text(c, ctrl_ls_end(false, &text), text);
        return;
    }
    if (!xfer_fs_ls(dir, r->ls_sizes, LS_CHUNK, ls_emit, c)) {
        ESP_LOGI(TAG, "%s: ls %s: cannot open it; an empty listing", c->ip, dir);
    }
    storage_release();
    uint8_t type = ctrl_ls_end(true, &text);
    ctrl_queue_big(c, type, text, strlen(text));
}

void ctrl_files_op(client_t *c, const uint8_t *payload, const ctrl_route_t *r)
{
    char a[XFER_PATH_MAX + 1], b[XFER_PATH_MAX + 1];
    if (!arg_path(payload, r, 0, a, sizeof(a)) || (r->nargs > 1 && !arg_path(payload, r, 1, b, sizeof(b)))) {
        ctrl_reject(c, "invalid path", true, false);
        return;
    }
    const char *why = write_refusal(a, false);
    if (!why && r->nargs > 1) {
        why = write_refusal(b, false);
    }
    if (why) {
        ctrl_reject(c, why, true, false);
        return;
    }
    if (r->action == CTRL_LOCAL_RM) {
        // Replies when done, from the worker.
        if (transfer_busy(c) || !run_job(c, JOB_RM, a)) {
            ctrl_refuse(c, TRANSFER_BUSY "\n", true, false);
        }
        return;
    }
    if (!storage_hold()) {
        ctrl_reject(c, "no SD card", true, false);
        return;
    }
    // Replies as stock words them.
    char text[2 * XFER_PATH_MAX + 32];
    bool ok;
    if (r->action == CTRL_LOCAL_MV) {
        ok = xfer_fs_mv(a, b);
        snprintf(text, sizeof(text), ok ? "renamed %s to %s\r\n" : "Could not rename %s to %s\r\n", a, b);
    } else {
        ok = xfer_fs_mkdir(a);
        snprintf(text, sizeof(text), ok ? "created directory %s\r\n" : "could not create directory %s\r\n", a);
    }
    storage_release();
    ESP_LOG_LEVEL_LOCAL(ok ? ESP_LOG_DEBUG : ESP_LOG_WARN, TAG, "%s: %.*s: %s", c->ip, (int)r->line_len,
                        (const char *)payload + r->line_off, ok ? "done" : "failed");
    ctrl_reply_text(c, PTYPE_NORMAL_INFO, text);
    if (ok) {
        ctrl_reply_text(c, PTYPE_LOAD_FINISH, "ok\r\n");
    } else {
        ctrl_reply_text(c, PTYPE_LOAD_ERROR, "");
    }
}

// ---- uploads

static bool ul_write(void *ctx, const uint8_t *buf, size_t n)
{
    client_t *c = ctx;
    esp_rom_md5_update(&c->ul_md5, buf, n);
    return fwrite(buf, 1, n, c->xfile) == n;
}

void ctrl_files_upload(client_t *c, const uint8_t *arg, size_t len)
{
    ctrl_files_close(c);
    if (transfer_busy(c)) {
        ctrl_busy_reply(c, TRANSFER_BUSY);
        return;
    }
    char path[XFER_PATH_MAX + 1];
    if (!xfer_path_decode((const char *)arg, len, path, sizeof(path))) {
        ctrl_reject(c, "invalid path", false, true);
        return;
    }
    const char *why = write_refusal(path, true);
    if (why) {
        ESP_LOGW(TAG, "%s: upload %s refused: %s", c->ip, path, why);
        ctrl_reject(c, why, false, true);
        return;
    }
    // Written under a hidden name and renamed when complete, so an aborted
    // upload never leaves a half file under the real one.
    const char *slash = strrchr(path, '/');
    if (!storage_hold()) {
        ctrl_reject(c, "no SD card", false, true);
        return;
    }
    c->held = true;
    c->uploading = true;
    atomic_fetch_add(&s_uploads, 1);
    if (ota_active()) {
        ctrl_files_close(c);
        ctrl_reject(c, "busy updating", false, true);
        return;
    }
    int n = snprintf(c->ul_tmp, sizeof(c->ul_tmp), "%.*s/.%s.part", (int)(slash - path), path, slash + 1);
    c->xfile = n > 0 && (size_t)n < sizeof(c->ul_tmp) ? fopen(c->ul_tmp, "wb") : NULL;
    if (!c->xfile) {
        c->ul_tmp[0] = '\0';
        ctrl_files_close(c);
        ESP_LOGW(TAG, "%s: upload %s: cannot create it", c->ip, path);
        ctrl_reject(c, "Invalid filename!", false, true);
        return;
    }
    snprintf(c->xfer_path, sizeof(c->xfer_path), "%s", path);
    esp_rom_md5_init(&c->ul_md5);
    c->xfer_last_us = esp_timer_get_time();
    xfer_ul_out_t o = xfer_ul_begin(&c->ul, s_xfer_out);
    ESP_LOGI(TAG, "%s: upload %s", c->ip, path);
    queue_out(c, o.type, o.len);
}

static void on_upload_frame(client_t *c, uint8_t type, const uint8_t *payload, size_t n)
{
    xfer_ul_out_t o = xfer_ul_on_frame(&c->ul, type, payload, n, ul_write, c, s_xfer_out);
    queue_out(c, o.type, o.len);
    if (o.complete) {
        char md5[33];
        md5_hex(&c->ul_md5, md5);
        bool same = !c->ul.md5[0] || !strcmp(md5, c->ul.md5);
        bool closed = fclose(c->xfile) == 0;
        c->xfile = NULL;
        bool kept = same && closed && xfer_fs_keep(c->ul_tmp, c->xfer_path, md5);
        if (kept) {
            c->ul_tmp[0] = '\0';  // it is the real file now
        }
        char text[XFER_PATH_MAX + 64];
        if (kept) {
            snprintf(text, sizeof(text), "Info: upload success: %s.\r\n", c->xfer_path);
        } else {
            snprintf(text, sizeof(text), "Error: upload of %s failed (%s).\r\n", c->xfer_path,
                     same ? "could not write it" : "MD5 mismatch");
        }
        ctrl_reply_text(c, PTYPE_NORMAL_INFO, text);  // before FILE_END "ok\r\n", as stock
        o = xfer_ul_finish(&c->ul, kept, s_xfer_out);
        queue_out(c, o.type, o.len);
        ESP_LOGI(TAG, "%s: upload %s %s (%lu bytes, md5 %s)", c->ip, c->xfer_path, kept ? "complete" : "failed",
                 (unsigned long)c->ul.received, md5);
        ctrl_files_close(c);
    } else if (o.failed) {
        if (type == PTYPE_FILE_CAN) {
            ctrl_reply_text(c, PTYPE_NORMAL_INFO, "Info: Upload canceled by remote!\r\n");  // stock's text
        }
        ESP_LOGW(TAG, "%s: upload %s %s", c->ip, c->xfer_path, type == PTYPE_FILE_CAN ? "cancelled" : "failed");
        ctrl_files_close(c);
    }
}

void ctrl_files_frame(client_t *c, uint8_t type, const uint8_t *payload, size_t n)
{
    if (c->ul.state != XFER_UL_IDLE) {
        c->xfer_last_us = esp_timer_get_time();
        on_upload_frame(c, type, payload, n);
        return;
    }
    if (c->hashing) {
        // The app waits for our FILE_MD5 first; only a cancel means anything.
        if (type == PTYPE_FILE_CAN) {
            ctrl_files_close(c);
        }
        return;
    }
    if (c->dl.state == XFER_DL_IDLE) {
        // A transfer frame with no transfer: tell the app to stop waiting.
        ctrl_lock();
        ctrl_queue_frame(c, PTYPE_FILE_CAN, NULL, 0);
        ctrl_unlock();
        return;
    }
    c->xfer_last_us = esp_timer_get_time();
    xfer_out_t o = xfer_dl_on_frame(&c->dl, type, payload, n, xfer_read, c->xfile, s_xfer_out);
    queue_out(c, o.type, o.len);
    if (o.done && type == PTYPE_FILE_END) {
        // Stock answers the app's FILE_END with "ok\r\n", then the text.
        char text[XFER_PATH_MAX + 40];
        snprintf(text, sizeof(text), "Info: download success: %s.\r\n", c->xfer_path);
        ctrl_lock();
        ctrl_queue_frame(c, PTYPE_FILE_END, "ok\r\n", 4);
        ctrl_queue_frame(c, PTYPE_NORMAL_INFO, text, strlen(text));
        ctrl_unlock();
    } else if (o.done && type == PTYPE_FILE_CAN) {
        // The app cancels when its cached copy has the same MD5. Stock string;
        // its use for downloads is INFERRED.
        ctrl_reply_text(c, PTYPE_NORMAL_INFO, "Info: canceled by remote!\r\n");
    }
    if (o.done) {
        ESP_LOGI(TAG, "%s: download %s", c->ip, type == PTYPE_FILE_END ? "complete" : "ended");
        ctrl_files_close(c);
    }
}
