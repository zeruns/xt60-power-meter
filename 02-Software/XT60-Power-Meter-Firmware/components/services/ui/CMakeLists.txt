# UI 服务：启动画面 + 测量/波形/设置三页（全部基于 LVGL 官方 widget）
idf_component_register(
    SRCS
        "src/ui.c"
        "src/ui_nav.c"
        "src/screens/ui_msgbox.c"
        "src/screens/splash.c"
        "src/screens/screen_measure.c"
        "src/screens/screen_wave.c"
        "src/screens/screen_settings.c"
    INCLUDE_DIRS "include" "src" "src/screens"
    # esp_app_format：启动画面读固件版本（esp_app_get_description）
    # power_measure：波形序列与时间窗/模式 API；key_button：按键事件路由
    # buzzer：按键操作反馈音；lcd_nv3030b：首帧上屏后打开背光
    # settings：用户偏好读写（设置页的响度/亮度持久化）
    # wifi_manager：WiFi 状态页与恢复出厂（F7/F6：状态查询/忘记网络）
    # lvgl__lvgl：LVGL 图形库（组件管理器托管，目标名为 <命名空间>__<组件名>）
    # 注意：按键驱动组件名用 key_button 而非 button —— 官方 esp_lvgl_port 的
    # CMake 会检测名为 "button" 的组件并编译其 iot_button 适配（esp_lvgl_port_button.c），
    # 与本工程自研按键驱动同名会导致它按 iot_button 的 API 编译而报错，故避开该名。
    REQUIRES lvgl__lvgl lvgl_port esp_app_format data_store power_measure key_button
             buzzer lcd_nv3030b settings orientation wifi_manager)
