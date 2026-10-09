/**
 * @file splash.c
 * @brief 启动画面实现（参考 RD/UM34 类桌面功率计的简洁风格）
 *
 * 版式（284×240 横屏，白底）：
 *   ┌──────────────────────────────┐  ← 顶部品牌色通栏（4px）
 *   │                              │
 *   │        XT60 功率计           │  Bold 34px，近黑
 *   │      作者博客：blog.zeruns    │  14px 中灰
 *   │        固件版本 v0.1.0        │  12px 浅灰
 *   │       ▓▓▓▓▓▓▓░░░░░            │  底部加载条（品牌色）
 *   └──────────────────────────────┘
 */
#include "splash.h"

#include "esp_app_desc.h"
#include "lvgl.h"
#include "lvgl_port.h"

/* 品牌与文字配色 */
#define COLOR_ACCENT  lv_color_hex(0x0066CC)
#define COLOR_TITLE   lv_color_hex(0x1A1A1A)
#define COLOR_SUB     lv_color_hex(0x666666)
#define COLOR_VER     lv_color_hex(0x999999)
#define COLOR_BAR_BG  lv_color_hex(0xE3E7EC)

static lv_obj_t *s_bar = NULL;

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    return lbl;
}

void splash_build(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);

    /* ① 顶部品牌色通栏 */
    lv_obj_t *topbar = lv_obj_create(scr);
    lv_obj_remove_style_all(topbar);
    lv_obj_set_size(topbar, LV_PCT(100), 4);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_style_bg_color(topbar, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(topbar, false);

    /* ② 中部纵向布局：小标识 + 标题 + 副标题 + 版本 */
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), 200);
    lv_obj_center(cont);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 8, 0);
    lv_obj_set_scrollable(cont, false);

    /* 英文小标识（蓝色大写字母间距感由空格模拟） */
    lv_obj_t *brand = make_label(cont, "X T 6 0   P O W E R   M E T E R",
                                 lvgl_port_get_font(LVGL_PORT_FONT_12), COLOR_ACCENT);
    lv_obj_set_style_text_letter_space(brand, 1, 0);
    lv_obj_set_style_pad_bottom(brand, 6, 0);

    /* 主标题：Bold 34px（字体文件不含时自动回退常规字重，不会方框） */
    make_label(cont, "XT60 功率计", lvgl_port_get_font_bold(LVGL_PORT_FONT_40), COLOR_TITLE);

    make_label(cont, "作者博客：blog.zeruns.com", lvgl_port_get_font(LVGL_PORT_FONT_16),
               COLOR_SUB);

    char ver_text[48];
    lv_snprintf(ver_text, sizeof(ver_text), "固件版本 v%s", esp_app_get_description()->version);
    make_label(cont, ver_text, lvgl_port_get_font(LVGL_PORT_FONT_14), COLOR_VER);

    /* ③ 底部加载条（宽 55%，高 6px，圆角胶囊） */
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 158, 6);
    lv_obj_set_pos(s_bar, (284 - 158) / 2, 204);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, COLOR_BAR_BG, 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bar, 3, 0);
    /* v9：bar 填充条是 INDICATOR 部件，用 bg_color/radius 样式控制 */
    lv_obj_set_style_bg_color(s_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_scrollable(s_bar, false);
}

void splash_set_progress(uint32_t pct)
{
    if (s_bar != NULL) {
        if (pct > 100) {
            pct = 100;
        }
        lv_bar_set_value(s_bar, (int32_t)pct, LV_ANIM_OFF);
    }
}
