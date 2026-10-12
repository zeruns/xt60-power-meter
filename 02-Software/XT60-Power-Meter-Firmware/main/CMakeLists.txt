idf_component_register(
    SRCS "main.c" "app_init.c" "app_tasks.c"
    INCLUDE_DIRS "."
    REQUIRES bsp event_bus data_store status_led buzzer system_state settings device_ctrl
            key_button battery_adc aht20 imu_sc7a20h ads131m02 power_measure
            fs_storage lcd_nv3030b touch_cst816d lvgl_port ui)
