idf_component_register(SRCS "src/wifi_policy.c" "src/wifi_mgr.c" "src/wifi_setup_ap.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board sdlog lwip mbedtls esp_wifi esp_netif esp_event esp_timer nvs_flash)
