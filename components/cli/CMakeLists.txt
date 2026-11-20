idf_component_register(SRCS "src/cli_args.c" "src/cli.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_app_format mbedtls wifi_mgr storage ctrl lpcfw esp_driver_usb_serial_jtag)
