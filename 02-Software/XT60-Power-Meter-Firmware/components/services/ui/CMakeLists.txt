# UI 服务：启动画面 + 测量/波形/设置三页（全部基于 LVGL 官方 widget）
idf_component_register(
    SRCS
        "src/ui.c"
        "src/ui_nav.c"
        "src/screens/splash.c"
        "src/screens/screen_measure.c"
        "src/screens/screen_wave.c"
        "src/screens/screen_settings.c"
    INCLUDE_DIRS "include" "src" "src/screens"
    # esp_app_format：启动画面读固件版本（esp_app_get_description）
    # power_measure：波形序列与时间窗/模式 API；button：按键事件路由
    # buzzer：按键操作反馈音；lcd_nv3030b：首帧上屏后打开背光
    REQUIRES lvgl lvgl_port esp_app_format data_store power_measure button
             buzzer lcd_nv3030b)
