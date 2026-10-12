idf_component_register(
    SRCS "src/aht20.c" "src/aht20_calc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c bsp)
