#pragma once
// Between wifi_mgr.c and wifi_setup_ap.c only.

// Serialises a radio reconfiguration with the other blocking operations
// (switching networks, scanning): pauses reconnects, keeps an association.
void wifi_mgr_op_begin(void);
void wifi_mgr_op_end(void);

// Sets up the setup hotspot (wifi_setup_ap.c); from wifi_mgr_start, before esp_wifi_start.
void wifi_setup_ap_init(void);
