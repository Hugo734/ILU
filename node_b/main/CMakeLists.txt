idf_component_register(
    SRCS "main.cpp" "hal_b_esp32.cpp"
    INCLUDE_DIRS "."
    REQUIRES core driver esp_timer
)