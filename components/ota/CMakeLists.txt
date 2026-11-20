idf_component_register(SRCS "src/ota.c" "src/ota_version.c" "src/http_origin.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES cfg wifi_mgr spi_flash app_update esp_app_format esp_http_server esp_netif)
