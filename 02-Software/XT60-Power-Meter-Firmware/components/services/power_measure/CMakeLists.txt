idf_component_register(
    SRCS "src/power_calc.c" "src/power_measure.c"
    INCLUDE_DIRS "include"
    REQUIRES ads131m02 aht20 battery_adc data_store event_bus settings system_state nvs_flash esp_timer esp_driver_tsens)
