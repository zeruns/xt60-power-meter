# esp_app_format：启动画面要读固件版本（esp_app_get_description）
idf_component_register(
    SRCS "src/ui.c"
    INCLUDE_DIRS "include"
    REQUIRES data_store lvgl_port lvgl esp_app_format)
