idf_component_register(
    SRCS "src/device_ctrl.c"
    INCLUDE_DIRS "include"
    REQUIRES event_bus status_led buzzer system_state settings)
