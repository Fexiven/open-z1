idf_component_register(SRCS "src/lpcfw_bundle.c" "src/lpcfw_sm.c" "src/lpcfw.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mkframe
                       PRIV_REQUIRES lpc_link machine ota play storage xfer esp_timer heap)
