idf_component_register(SRCS "src/discovery_fmt.c" "src/discovery.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES machine wifi_mgr esp_netif lwip)
