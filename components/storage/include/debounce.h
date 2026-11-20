#pragma once
// Pure, host-tested: a two-state input that changes only after DEBOUNCE_SAMPLES
// equal samples in a row (the card detect switch, sampled every 500 ms).

#include <stdbool.h>

#define DEBOUNCE_SAMPLES 3

typedef struct {
    bool stable, last;
    int same;
} debounce_t;

void debounce_init(debounce_t *d, bool state);

// Feeds one sample; returns the debounced state.
bool debounce_feed(debounce_t *d, bool sample);
