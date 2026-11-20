idf_component_register(SRCS "src/stats_fmt.c" "src/stats.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board nvs_flash esp_timer)
