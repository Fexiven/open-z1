idf_component_register(SRCS "src/camera.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board wifi_mgr esp_http_server esp_timer lwip espressif__esp32-camera)
