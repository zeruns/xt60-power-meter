idf_component_register(
    SRCS "src/touch_cst816d.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp esp_driver_i2c esp_driver_gpio)
