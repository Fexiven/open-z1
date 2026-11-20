idf_component_register(SRCS "src/lpc_link.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mkframe
                       PRIV_REQUIRES health esp_driver_uart esp_driver_gpio)
