idf_component_register(
    SRCS "src/web_console.c" "src/web_console_page.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server data_store esp_timer)
