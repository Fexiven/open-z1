idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES camera cfg play lpc_link lpcfw cli ctrl wifi_mgr discovery machine stats sdlog storage ota webui health nvs_flash esp_app_format esp_driver_gpio esp_timer)
