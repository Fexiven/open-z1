idf_component_register(SRCS "src/sdlog_ring.c" "src/sdlog.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES storage esp_timer)
