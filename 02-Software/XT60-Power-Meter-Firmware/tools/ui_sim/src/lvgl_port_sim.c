/**
 * @file lvgl_port_sim.c
 * @brief 模拟器的 lvgl_port 实现（与真机同接口、同字体加载路径）
 *
 * 与真机（components/middleware/lvgl_port/src/lvgl_port.c）保持一致的地方：
 *   · 同一份 Noto Sans SC TTF（notosc_reg / notosc_bold）用 lv_tiny_ttf 建
 *     8 档字号 × 2 字重 —— 字形光栅化结果与真机相同；
 *   · fallback 挂 montserrat_14（图标符号走回退链，与真机一致）；
 *   · 用 16px Noto 重建默认主题（msgbox/roller 等控件默认字体一致）；
 *   · 旋转切换同步 LVGL 逻辑分辨率（真机面板方向固定，同样靠 LVGL 软件旋转，
 *     运行期不写 MADCTL）。
 */
#include "lvgl_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sim.h"

static const char *TAG = "lvgl_sim";

/* 字体路径由 CMake 以绝对路径注入（指向工程内同一份 TTF） */
#ifndef SIM_FONT_REG_PATH
#define SIM_FONT_REG_PATH "notosc_reg.ttf"
#endif
#ifndef SIM_FONT_BOLD_PATH
#define SIM_FONT_BOLD_PATH "notosc_bold.ttf"
#endif

enum { FONT_REG = 0, FONT_BOLD, FONT_WEIGHT_MAX };

/** 各字号像素高度（与真机 lvgl_port.c 完全一致） */
static const int s_font_px[LVGL_PORT_FONT_MAX] = { 12, 14, 16, 20, 24, 32, 36, 40 };

static lv_font_t *s_fonts[FONT_WEIGHT_MAX][LVGL_PORT_FONT_MAX] = { 0 };
static uint8_t *s_font_data[FONT_WEIGHT_MAX] = { 0 };
static size_t s_font_data_len[FONT_WEIGHT_MAX] = { 0 };

static uint32_t s_flush_count = 0;
static lcd_nv3030b_rotation_t s_rotation = LCD_NV3030B_ROT_90;
static bool s_inited = false;

/* 由 sim_main 的 flush 回调调用（见 sim.h） */
void sim_flush_count_inc(void)
{
    s_flush_count++;
}

/** @brief 读整个文件到内存（模拟器不需要 PSRAM，直接 malloc） */
static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "字体文件打不开: %s（将回退内置 ASCII 字体）", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)sz;
    return buf;
}

/**
 * @brief 加载中文字体（与真机 load_fonts 相同的构建方式）
 *
 * 真机在后台任务里加载并逐字号取 LVGL 锁；模拟器是单线程，直接同步加载。
 * 两条路径用同一份 TTF 与同一组像素高度，所以字形位图一致。
 */
static void load_fonts(void)
{
    const struct {
        const char *path;
    } faces[FONT_WEIGHT_MAX] = {
        { SIM_FONT_REG_PATH },
        { SIM_FONT_BOLD_PATH },
    };

    int ok_count = 0;
    for (int w = 0; w < FONT_WEIGHT_MAX; w++) {
        s_font_data[w] = read_file(faces[w].path, &s_font_data_len[w]);
        if (s_font_data[w] == NULL) {
            continue;
        }
        for (int i = 0; i < LVGL_PORT_FONT_MAX; i++) {
            s_fonts[w][i] =
                lv_tiny_ttf_create_data(s_font_data[w], s_font_data_len[w], s_font_px[i]);
            if (s_fonts[w][i] != NULL) {
                ok_count++;
            }
        }
    }

    /* 图标符号回退链（与真机一致） */
    for (int w = 0; w < FONT_WEIGHT_MAX; w++) {
        for (int i = 0; i < LVGL_PORT_FONT_MAX; i++) {
            if (s_fonts[w][i] != NULL) {
                s_fonts[w][i]->fallback = &lv_font_montserrat_14;
            }
        }
    }

    ESP_LOGI(TAG, "中文字体加载完成：%d/%d（模拟器直接读取工程内 TTF）", ok_count,
             FONT_WEIGHT_MAX * LVGL_PORT_FONT_MAX);
}

esp_err_t lvgl_port_app_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    load_fonts();

    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        ESP_LOGE(TAG, "尚无显示设备（应先创建 display 再调用本函数）");
        return ESP_FAIL;
    }

    /* 用中文字体重建默认主题（与真机一致：主色 0x0066CC） */
    if (s_fonts[FONT_REG][LVGL_PORT_FONT_16] != NULL) {
        lv_theme_default_init(disp, lv_color_hex(0x0066CC), lv_color_hex(0x8A8F99),
                              false /*浅色主题*/, s_fonts[FONT_REG][LVGL_PORT_FONT_16]);
    }

    s_inited = true;
    return ESP_OK;
}

static const lv_font_t *get_font(int weight, lvgl_port_font_size_t size)
{
    if (size < 0 || size >= LVGL_PORT_FONT_MAX || weight < 0 || weight >= FONT_WEIGHT_MAX) {
        return NULL;
    }
    lv_font_t *font = s_fonts[weight][size];
    if (font != NULL) {
        return font;
    }
    return &lv_font_montserrat_14; /* 与真机相同的回退策略 */
}

const lv_font_t *lvgl_port_get_font(lvgl_port_font_size_t size)
{
    return get_font(FONT_REG, size);
}

const lv_font_t *lvgl_port_get_font_bold(lvgl_port_font_size_t size)
{
    return get_font(FONT_BOLD, size);
}

/* 模拟器没有"嵌入子集/全量字库"之分：两者映射到同一套已加载的全量字体，
 * 启动页与真机一样直接走中文分支（用于验证中文文案与版式）。 */
const lv_font_t *lvgl_port_get_splash_font(lvgl_port_font_size_t size)
{
    return get_font(FONT_REG, size);
}

const lv_font_t *lvgl_port_get_splash_font_bold(lvgl_port_font_size_t size)
{
    return get_font(FONT_BOLD, size);
}

bool lvgl_port_splash_fonts_ready(void)
{
    return true;
}

bool lvgl_port_fonts_ready(void)
{
    return true; /* 模拟器同步加载，启动页直接走中文分支 */
}

uint32_t lvgl_port_flush_count(void)
{
    return s_flush_count;
}

esp_err_t lvgl_port_set_rotation(lcd_nv3030b_rotation_t rot)
{
    if (rot > LCD_NV3030B_ROT_270) {
        return ESP_ERR_INVALID_ARG;
    }
    int w = 0;
    int h = 0;
    lcd_nv3030b_get_resolution(rot, &w, &h);

    s_rotation = rot;

    lv_display_t *d = lv_display_get_default();
    if (d != NULL) {
        /* 与真机同路径：改逻辑分辨率，LVGL 广播 RESOLUTION_CHANGED 自动重排界面 */
        lv_display_set_resolution(d, w, h);
    }
    ESP_LOGI(TAG, "显示旋转已切换为 ROT_%d（%dx%d）", rot * 90, w, h);
    return ESP_OK;
}

lcd_nv3030b_rotation_t lvgl_port_get_rotation(void)
{
    return s_rotation;
}

/* 角度↔枚举（与真机实现完全一致） */
static lcd_nv3030b_rotation_t sim_angle_to_rot(uint16_t deg)
{
    switch (deg) {
    case 90:  return LCD_NV3030B_ROT_180;
    case 180: return LCD_NV3030B_ROT_270;
    case 270: return LCD_NV3030B_ROT_0;
    default:  return LCD_NV3030B_ROT_90;
    }
}

static uint16_t sim_rot_to_angle(lcd_nv3030b_rotation_t rot)
{
    switch (rot) {
    case LCD_NV3030B_ROT_180: return 90;
    case LCD_NV3030B_ROT_270: return 180;
    case LCD_NV3030B_ROT_0:   return 270;
    default:                  return 0;
    }
}

esp_err_t lvgl_port_set_rotation_angle(uint16_t deg)
{
    if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
        return ESP_ERR_INVALID_ARG;
    }
    return lvgl_port_set_rotation(sim_angle_to_rot(deg));
}

uint16_t lvgl_port_get_rotation_angle(void)
{
    return sim_rot_to_angle(s_rotation);
}

/* 单线程模拟器：锁为直通（真机由官方 esp_lvgl_port 提供互斥） */
bool lvgl_port_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}

void lvgl_port_unlock(void)
{
}