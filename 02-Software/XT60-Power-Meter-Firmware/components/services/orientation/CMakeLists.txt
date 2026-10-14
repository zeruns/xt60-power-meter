idf_component_register(
    SRCS "src/orientation.c"
    INCLUDE_DIRS "include"
    REQUIRES imu_sc7a20h lvgl_port settings)
