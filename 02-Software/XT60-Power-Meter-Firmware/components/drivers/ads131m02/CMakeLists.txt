idf_component_register(
    SRCS "src/ads131m02.c" "src/ads131m02_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio bsp
    PRIV_REQUIRES esp_timer esp_driver_dma esp_hal_gpspi esp_hal_dma esp_hw_support)
