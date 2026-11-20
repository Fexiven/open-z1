idf_component_register(SRCS "src/storage.c" "src/debounce.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board fatfs sdmmc esp_driver_sdmmc esp_driver_gpio)
