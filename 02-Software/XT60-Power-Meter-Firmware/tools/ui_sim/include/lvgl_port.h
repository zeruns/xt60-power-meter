/**
 * @file lvgl_port.h
 * @brief 模拟器桩：应用侧 LVGL 适配层接口（与真机同名同形）
 *
 * 真机版基于 ESP 官方组件 espressif/esp_lvgl_port；模拟器在 PC 上以单线程
 * 渲染循环复刻同类能力：
 *   · 字体：从工程内同一份 Noto Sans SC TTF（notosc_reg/bold.ttf）用
 *     lv_tiny_ttf 建同样 8 档字号 —— 与真机加载路径一致，保证字形光栅化结果相同；
 *   · 锁：单线程下为直通实现（返回 true / 空操作）；
 *   · 旋转：记录档位并同步 LVGL 逻辑分辨率（真机同样由 LVGL 软件旋转完成，
 *     运行期不向面板写 MADCTL，面板物理方向固定为横屏）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#include "lcd_nv3030b.h"

/** 常用字号枚举（与真机 lvgl_port.h 完全一致） */
typedef enum {
    LVGL_PORT_FONT_12 = 0,
    LVGL_PORT_FONT_14,
    LVGL_PORT_FONT_16,
    LVGL_PORT_FONT_20,
    LVGL_PORT_FONT_24,
    LVGL_PORT_FONT_32,
    LVGL_PORT_FONT_36, /* 首页大数字用（与真机同步新增） */
    LVGL_PORT_FONT_40,
    LVGL_PORT_FONT_MAX,
} lvgl_port_font_size_t;

/** @brief 初始化字体与主题（在 lv_init 与显示注册之后调用；对应真机 lvgl_port_app_init） */
esp_err_t lvgl_port_app_init(void);

const lv_font_t *lvgl_port_get_font(lvgl_port_font_size_t size);
const lv_font_t *lvgl_port_get_font_bold(lvgl_port_font_size_t size);

/** 启动页嵌入子集（模拟器与全量字库同源，便于验证中文文案/版式） */
const lv_font_t *lvgl_port_get_splash_font(lvgl_port_font_size_t size);
const lv_font_t *lvgl_port_get_splash_font_bold(lvgl_port_font_size_t size);
bool lvgl_port_splash_fonts_ready(void);

/** 模拟器字体在初始化时同步加载完成，恒为 true（真机为后台异步加载） */
bool lvgl_port_fonts_ready(void);
uint32_t lvgl_port_flush_count(void);

esp_err_t lvgl_port_set_rotation(lcd_nv3030b_rotation_t rot);
lcd_nv3030b_rotation_t lvgl_port_get_rotation(void);

/* 角度 API（与真机同语义：0°=默认横屏，顺时针） */
esp_err_t lvgl_port_set_rotation_angle(uint16_t deg);
uint16_t lvgl_port_get_rotation_angle(void);

/** 以下两个由真机 esp_lvgl_port 提供；模拟器为单线程直通实现 */
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);