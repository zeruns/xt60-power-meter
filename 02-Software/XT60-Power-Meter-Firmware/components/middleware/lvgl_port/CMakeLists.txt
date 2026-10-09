idf_component_register(
    SRCS "src/lvgl_port.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl lcd_nv3030b touch_cst816d esp_timer)
# 中文字体已改为从 SPIFFS（storage 分区）文件加载：
#   - 字体文件在 fs_storage 组件的 spiffs_image/fonts/ 下，随镜像烧入；
#   - 启动表保证 fs_storage_init() 先于 lvgl_port_init() 完成挂载；
#   - 本组件不再 EMBED_FILES，app 程序分区不含字体数据。
