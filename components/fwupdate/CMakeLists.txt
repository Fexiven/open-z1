idf_component_register(SRCS "src/fwupdate.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ota lpcfw ctrl storage xfer esp_http_client esp_https_ota esp_http_server
                                     mbedtls esp_timer app_update esp_app_format heap)
