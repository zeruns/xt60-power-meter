# 显示驱动组件
#
# REQUIRES 说明：
#   · esp_lcd / esp_driver_spi / esp_driver_ledc / esp_driver_gpio：QSPI 总线、
#     panel IO、LEDC 背光与复位脚；
#   · bsp：引脚唯一真相源（board_pins.h）；
#   · espressif__esp_lcd_spd2010：官方面板驱动组件，提供该屏的分页式初始化
#     序列与 QSPI 命令 opcode 封装（见 src/lcd_nv3030b.c 文件头踩坑记录）。
#     组件名由组件管理器按 "<命名空间>__<组件名>" 生成，依赖声明见 idf_component.yml。
idf_component_register(
    SRCS "src/lcd_nv3030b.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp esp_lcd esp_driver_spi esp_driver_ledc esp_driver_gpio
             espressif__esp_lcd_spd2010)
