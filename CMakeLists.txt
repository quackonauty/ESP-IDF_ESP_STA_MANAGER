idf_component_register(
    SRCS "esp_sta_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_wifi
        wifi_provisioning
)