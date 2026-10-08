idf_component_register(
    SRCS "src/ads131m02.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_gpio bsp)
