idf_component_register(
    SRCS "main.c" "app_tasks.c"
    INCLUDE_DIRS "."
    REQUIRES bsp status_led system_state)
