#pragma once
// Pure byte ring for one client's outgoing frames. Frames go in whole or not
// at all (a partial frame would corrupt the stream); the socket drains it in
// whatever pieces it accepts.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;  // next byte to send
    size_t len;   // bytes queued
} ctrl_txq_t;

void ctrl_txq_init(ctrl_txq_t *q, uint8_t *buf, size_t cap);

// Queues n bytes if they all fit; returns false (queue unchanged) otherwise.
bool ctrl_txq_put(ctrl_txq_t *q, const void *data, size_t n);

// Returns the length of the contiguous run of queued bytes starting at *out.
size_t ctrl_txq_peek(const ctrl_txq_t *q, const uint8_t **out);

// Drops n bytes (n <= queued) from the front after they were sent.
void ctrl_txq_consume(ctrl_txq_t *q, size_t n);
