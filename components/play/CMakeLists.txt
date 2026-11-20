idf_component_register(SRCS "src/play_sm.c" "src/play.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mkframe
                       PRIV_REQUIRES esp_timer health lpc_link machine ota storage xfer)
