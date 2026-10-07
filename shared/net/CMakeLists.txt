idf_component_register(
    SRCS "net.cpp"
    INCLUDE_DIRS "."
    REQUIRES core mqtt esp_wifi esp_event nvs_flash esp_netif esp_timer
)
