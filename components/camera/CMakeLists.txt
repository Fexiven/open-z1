idf_component_register(SRCS "src/camera.c" "src/camera_res.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board nvs_flash wifi_mgr esp_http_server lwip esp_timer espressif__esp32-camera)
