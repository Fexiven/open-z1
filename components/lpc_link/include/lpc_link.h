#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "mkframe.h"

// Starts the UART link to the LPC1768. cb runs on the link's RX task for
// every valid frame.
esp_err_t lpc_link_start(mk_frame_cb_t cb, void *ctx);

// Payloads up to LPC_MAX_TX_PAYLOAD, FIRM_DATA (for the LPC's bootloader) up
// to LPC_FIRM_TX_PAYLOAD.
esp_err_t lpc_link_send(uint8_t type, const void *payload, size_t n);

// While on, console traffic (CTRL_SINGLE, CTRL_MULTI: the keepalive, the
// app's commands, Stop/Pause) is refused with ESP_ERR_INVALID_STATE, so the
// LPC's bootloader hears only answers to its own requests during a firmware
// transfer (stock pauses its polling then too).
void lpc_link_console_block(bool on);

// A '?' status query. Main's housekeeping sends one every 500 ms: the LPC
// raises an alarm after 10 s without a packet.
void lpc_link_keepalive(void);
