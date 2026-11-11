/**
 * @file mock_lcd.c
 * @brief 显示驱动 mock：旋转档位 / 分辨率 / 背光亮度（不参与渲染，只记录状态）
 */
#include "lcd_nv3030b.h"

#include "esp_log.h"

static const char *TAG = "lcd_sim";

static lcd_nv3030b_rotation_t s_rot = LCD_NV3030B_ROT_90;
static uint8_t s_bright = 80;

esp_err_t lcd_nv3030b_set_rotation(lcd_nv3030b_rotation_t rot)
{
    if (rot > LCD_NV3030B_ROT_270) {
        return ESP_ERR_INVALID_ARG;
    }
    s_rot = rot;
    return ESP_OK;
}

lcd_nv3030b_rotation_t lcd_nv3030b_get_rotation(void)
{
    return s_rot;
}

/** 竖屏档（ROT_0/ROT_180）返回 240×284，横屏档（ROT_90/ROT_270）返回 284×240 */
void lcd_nv3030b_get_resolution(lcd_nv3030b_rotation_t rot, int *width, int *height)
{
    bool portrait = (rot == LCD_NV3030B_ROT_0 || rot == LCD_NV3030B_ROT_180);
    if (width != NULL) {
        *width = portrait ? LCD_NV3030B_PHYS_W : LCD_NV3030B_H_RES;
    }
    if (height != NULL) {
        *height = portrait ? LCD_NV3030B_PHYS_H : LCD_NV3030B_V_RES;
    }
}

esp_err_t lcd_nv3030b_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_bright = percent;
    ESP_LOGI(TAG, "背光亮度 → %u%%（模拟：不改变渲染内容）", (unsigned)percent);
    return ESP_OK;
}