idf_component_register(SRCS "src/webui.c" "src/webui_fmt.c"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "www/index.html" "www/setup.html"
                       PRIV_REQUIRES ota camera mbedtls cfg ctrl health lpc_link machine play sdlog storage wifi_mgr app_update esp_app_format esp_timer esp_http_server)
