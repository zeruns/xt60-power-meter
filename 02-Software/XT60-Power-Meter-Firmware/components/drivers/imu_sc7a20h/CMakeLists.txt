idf_component_register(
    SRCS "src/imu_sc7a20h.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c bsp)
