idf_component_register(
    SRCS "src/web_console.c" "src/web_console_page.c"
    INCLUDE_DIRS "include"
    # settings：v0.6.2 需求 13 的 GET /api/config 需回读用户配置（只读依赖）
    # esp_app_format：GET /api/info 用 esp_app_get_description() 读固件版本
    # power_measure：v0.6.6 起 GET /api/config 回读标定参数、POST 改标定/清零（同 settings 的先例）
    REQUIRES esp_http_server data_store esp_timer wifi_manager settings power_measure esp_app_format)
