idf_component_register(SRCS "src/ctrl_route.c" "src/ctrl_txq.c" "src/ctrl_server.c" "src/ctrl_files.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mkframe
                       PRIV_REQUIRES health vfs mbedtls ota cfg lpc_link lpcfw play wifi_mgr stats storage xfer lwip esp_timer esp_app_format esp_rom)
