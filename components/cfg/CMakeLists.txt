idf_component_register(SRCS "src/cfg_text.c" "src/cfg.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mkframe
                       PRIV_REQUIRES lpc_link storage xfer)
