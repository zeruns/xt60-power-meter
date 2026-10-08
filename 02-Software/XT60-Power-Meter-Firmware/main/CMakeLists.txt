idf_component_register(
    SRCS "main.c" "app_init.c" "app_tasks.c"
    INCLUDE_DIRS "."
    REQUIRES bsp event_bus data_store status_led buzzer system_state settings device_ctrl)
