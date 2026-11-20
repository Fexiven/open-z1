#include "debounce.h"

void debounce_init(debounce_t *d, bool state)
{
    d->stable = d->last = state;
    d->same = DEBOUNCE_SAMPLES;
}

bool debounce_feed(debounce_t *d, bool sample)
{
    if (sample != d->last) {
        d->last = sample;
        d->same = 0;
    }
    if (d->same < DEBOUNCE_SAMPLES) {
        d->same++;
    }
    if (d->same >= DEBOUNCE_SAMPLES) {
        d->stable = sample;
    }
    return d->stable;
}
