idf_component_register(
    SRCS "src/buzzer.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_ledc esp_timer bsp)
