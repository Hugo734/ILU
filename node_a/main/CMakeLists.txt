idf_component_register(SRCS "main.cpp" "hal_a_esp32.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES core net driver esp_timer)