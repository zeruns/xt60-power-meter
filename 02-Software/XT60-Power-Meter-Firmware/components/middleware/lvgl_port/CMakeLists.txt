# LVGL 应用侧适配层
#
# 依赖说明：
#   · espressif__esp_lvgl_port：ESP 官方 LVGL 移植层（组件管理器托管，
#     声明见 idf_component.yml，版本记录在工程根 dependencies.lock）；
#   · lvgl__lvgl            ：LVGL 图形库本体（同样由组件管理器托管）；
#   · lcd_nv3030b / touch_cst816d：本工程显示与触摸驱动。
#
# 注：托管组件的 CMake 目标名为「<命名空间>__<组件名>」，因此是
#     espressif__esp_lvgl_port 与 lvgl__lvgl（而非 espressif/esp_lvgl_port）。
idf_component_register(
    SRCS "src/lvgl_port.c"
    INCLUDE_DIRS "include"
    REQUIRES lcd_nv3030b touch_cst816d espressif__esp_lvgl_port lvgl__lvgl)
# 中文字体已改为从 SPIFFS（storage 分区）文件加载：
#   - 字体文件在 fs_storage 组件的 spiffs_image/fonts/ 下，随镜像烧入；
#   - 启动表保证 fs_storage_init() 先于 lvgl_port_app_init() 完成挂载；
#   - 本组件不再 EMBED_FILES，app 程序分区不含字体数据。
