/**
 * @file lcd_nv3030b.h
 * @brief 模拟器桩：显示驱动接口（与真机同名同形，仅保留 UI/设置页需要的部分）
 *
 * 说明：真机版还包含 esp_lcd 面板句柄等接口（依赖 ESP-IDF，模拟器不可用），
 * 但 UI 层只用到旋转档位/分辨率/背光亮度这几个能力，故此桩只需覆盖这些。
 * 保持与真机一致的类型名与函数签名，UI 代码可同源编译。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** 面板物理玻璃尺寸（竖屏原生 240 宽 × 284 高） */
#define LCD_NV3030B_PHYS_W 240
#define LCD_NV3030B_PHYS_H 284

/** 默认（横屏）逻辑分辨率 */
#define LCD_NV3030B_H_RES 284
#define LCD_NV3030B_V_RES 240

/** 显示旋转档位（与真机 lcd_nv3030b.h 完全一致） */
typedef enum {
    LCD_NV3030B_ROT_0 = 0, /**< 竖屏（物理原生） */
    LCD_NV3030B_ROT_90,    /**< 横屏（默认） */
    LCD_NV3030B_ROT_180,   /**< 竖屏倒置 */
    LCD_NV3030B_ROT_270,   /**< 横屏倒置 */
} lcd_nv3030b_rotation_t;

esp_err_t lcd_nv3030b_set_rotation(lcd_nv3030b_rotation_t rot);
lcd_nv3030b_rotation_t lcd_nv3030b_get_rotation(void);
void lcd_nv3030b_get_resolution(lcd_nv3030b_rotation_t rot, int *width, int *height);

/**
 * @brief 设置背光亮度（模拟器实现：记录值并打印；背光调光不影响渲染内容）
 * @param percent 亮度百分比 0~100
 */
esp_err_t lcd_nv3030b_set_backlight(uint8_t percent);