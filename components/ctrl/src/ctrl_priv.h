#pragma once
// Shared by ctrl_server.c (sockets, output queues, dispatch) and ctrl_files.c
// (file transfers and file commands). Not a public API.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ctrl_route.h"
#include "ctrl_txq.h"
#include "esp_err.h"
#include "esp_rom_md5.h"
#include "mkframe.h"
#include "sdkconfig.h"
#include "xfer_dl.h"
#include "xfer_path.h"
#include "xfer_ul.h"

// The largest app frame we act on: an upload packet (seq + 8192 bytes). Each
// client slot holds one such buffer: about 33 KB of RAM for 4 clients.
#define CLIENT_FRAME_MAX (4 + XFER_UL_PACKET_MAX)
// Download packet size, as stock (8192): the app asks for
// one packet per round trip. A frame of it must fit the per-client output
// queue alongside the status frames that keep coming (CTRL_TXQ_SIZE).
#define XFER_PACKET 8192
#define FRAME_PAYLOAD_MAX (4 + XFER_PACKET)

// Stock's text when another client holds the file transfer (stock strings).
#define TRANSFER_BUSY "Other client is currently uploading/downloading files. Please try again later."

typedef struct {
    int fd;  // -1: free slot
    char ip[16];
    uint32_t order;  // connect number: a worker's reply never reaches a newcomer in the same slot
    bool dead;
    int64_t last_rx_us;
    int64_t partial_since_us;     // 0: no partly received frame
    int64_t version_deadline_us;  // 0: no `version` waiting for FIRM_VER
    unsigned status_drops;
    mk_parser_t parser;
    uint8_t rxbuf[CLIENT_FRAME_MAX + MK_FRAME_OVERHEAD];
    ctrl_txq_t txq;
    uint8_t *txbuf;
    // The rest belongs to ctrl_files.c (server task only).
    xfer_dl_t dl;          // download in progress, if dl.state != XFER_DL_IDLE
    xfer_ul_t ul;          // upload in progress, if ul.state != XFER_UL_IDLE
    bool hashing;          // a download waits for the file worker's MD5
    uint32_t dl_size;      // its size, meanwhile
    FILE *xfile;           // the file of either
    bool held;             // xfile holds the card (storage_hold)
    bool uploading;        // counted for ctrl_server_uploading
    char xfer_path[XFER_PATH_MAX + 1];  // of the upload or download
    char ul_tmp[XFER_PATH_MAX + 8];     // where it is written until complete; "" when gone
    md5_context_t ul_md5;
    int64_t xfer_last_us;
} client_t;

extern client_t ctrl_clients[CONFIG_Z1_CTRL_MAX_CLIENTS];

// ---- ctrl_server.c

// The lock guards the slot table (fd, dead, order), every output queue and the
// wake flag.
void ctrl_lock(void);
void ctrl_unlock(void);
// Caller holds the lock. Makes the server task run its loop now.
void ctrl_wake(void);
// Caller holds the lock. Queues one frame for c (and wakes the server task).
void ctrl_queue_frame(client_t *c, uint8_t type, const void *payload, size_t n);
// Queues a frame for a long reply, first sending queued output until it fits
// (server task only).
void ctrl_queue_big(client_t *c, uint8_t type, const void *payload, size_t n);
void ctrl_reply_text(client_t *c, uint8_t type, const char *text);
// text as is, then LOAD_ERROR (load_reply) and/or FILE_CAN (file_cancel).
void ctrl_refuse(client_t *c, const char *text, bool load_reply, bool file_cancel);
// "error: <reason>\n", then as ctrl_refuse.
void ctrl_reject(client_t *c, const char *reason, bool load_reply, bool file_cancel);
// Stock's 0x91 refusal for one client.
void ctrl_busy_reply(client_t *c, const char *text);
// Caller holds the lock: the client in slot is still the one with that order.
bool ctrl_client_is(int slot, uint32_t order);

// ---- ctrl_files.c (server task, except where noted)

esp_err_t ctrl_files_start(void);
void ctrl_files_download(client_t *c, const uint8_t *arg, size_t len);
void ctrl_files_upload(client_t *c, const uint8_t *arg, size_t len);
// Any other FILE_* frame from c.
void ctrl_files_frame(client_t *c, uint8_t type, const uint8_t *payload, size_t n);
void ctrl_files_ls(client_t *c, const uint8_t *payload, const ctrl_route_t *r);
// rm, mv, mkdir.
void ctrl_files_op(client_t *c, const uint8_t *payload, const ctrl_route_t *r);
// FILE_PLAYING (0xB7).
void ctrl_files_playing(client_t *c);
// Ends c's transfer, if any; an unfinished upload leaves nothing behind.
void ctrl_files_close(client_t *c);
// Once per server loop: abandoned transfers, and a finished MD5 job.
void ctrl_files_poll(int64_t now);
