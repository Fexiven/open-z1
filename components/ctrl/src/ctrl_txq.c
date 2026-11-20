#include "ctrl_txq.h"

#include <string.h>

void ctrl_txq_init(ctrl_txq_t *q, uint8_t *buf, size_t cap)
{
    q->buf = buf;
    q->cap = cap;
    q->head = 0;
    q->len = 0;
}

bool ctrl_txq_put(ctrl_txq_t *q, const void *data, size_t n)
{
    if (n > q->cap - q->len) {
        return false;
    }
    size_t tail = (q->head + q->len) % q->cap;
    size_t first = q->cap - tail < n ? q->cap - tail : n;
    memcpy(q->buf + tail, data, first);
    memcpy(q->buf, (const uint8_t *)data + first, n - first);
    q->len += n;
    return true;
}

size_t ctrl_txq_peek(const ctrl_txq_t *q, const uint8_t **out)
{
    *out = q->buf + q->head;
    size_t run = q->cap - q->head;
    return q->len < run ? q->len : run;
}

void ctrl_txq_consume(ctrl_txq_t *q, size_t n)
{
    q->head = (q->head + n) % q->cap;
    q->len -= n;
    if (q->len == 0) {
        q->head = 0;
    }
}
