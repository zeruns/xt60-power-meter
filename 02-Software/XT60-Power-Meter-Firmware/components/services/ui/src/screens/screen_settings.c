/**
 * @file screen_settings.c
 * @brief 设置页框架：lv_list 列出设置项，功能占位提示用模态对话框
 *
 * ────────────────────────────────────────────────────────────
 * 踩坑记录：为什么"点确定没反应"（重要）
 * ────────────────────────────────────────────────────────────
 * LVGL 9 在创建显示设备时对顶层图层做了 `lv_obj_set_clickable(top_layer, false)`
 * （见 src/display/lv_display.c）。**不可点击的容器，其整棵子树都不参与命中
 * 测试**，因此挂在 `lv_layer_top()` 上的 lv_msgbox 虽然画得出来，但它的按钮
 * 永远收不到按下/点击事件——表现就是"对话框关不掉"。
 * 解决：显示对话框期间把顶层图层临时设为可点击（同时也顺便让它吞掉
 * 底层 UI 的触摸，形成真正的模态）；对话框删除后若图层已无子对象，再恢复。
 * ────────────────────────────────────────────────────────────
 */
#include "screen_settings.h"

#include <stdio.h>

#include "esp_app_desc.h"
#include "lvgl.h"
#include "lvgl_port.h"

/** 对话框样式与关闭处理是否已安装（顶层图层只需装一次） */
static bool s_layer_hooked = false;

/**
 * @brief 点击顶层图层空白处 → 关闭当前对话框（点遮罩关闭）
 */
static void layer_clicked_cb(lv_event_t *e)
{
    lv_obj_t *layer = lv_event_get_current_target(e);
    uint32_t n = lv_obj_get_child_count(layer);
    if (n > 0) {
        lv_msgbox_close_async(lv_obj_get_child(layer, n - 1));
    }
}

/**
 * @brief 对话框被删除 → 若图层已无子对象则恢复"不可点击"，避免吞掉主界面触摸
 */
static void box_deleted_cb(lv_event_t *e)
{
    lv_obj_t *layer = (lv_obj_t *)lv_event_get_user_data(e);
    if (layer != NULL && lv_obj_get_child_count(layer) == 0) {
        lv_obj_set_clickable(layer, false);
    }
}

/**
 * @brief 把对话框里的文字全部换成中文字体（主题字体已是 Noto，这里是双保险）
 */
static void style_msgbox(lv_obj_t *box)
{
    lv_obj_t *title = lv_msgbox_get_title(box);
    if (title != NULL) {
        lv_obj_set_style_text_font(title, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    }
    lv_obj_t *content = lv_msgbox_get_content(box);
    if (content != NULL) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(content); i++) {
            lv_obj_set_style_text_font(lv_obj_get_child(content, i),
                                       lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(content, i),
                                        lv_color_hex(0x374151), 0);
        }
    }
    lv_obj_t *footer = lv_msgbox_get_footer(box);
    if (footer != NULL) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(footer); i++) {
            lv_obj_t *btn = lv_obj_get_child(footer, i);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl != NULL) {
                lv_obj_set_style_text_font(lbl, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
            }
            lv_obj_set_style_min_width(btn, 84, 0);
        }
    }
}

/**
 * @brief 弹出模态提示框（官方 lv_msgbox，放在可点击的顶层图层上）
 */
static void show_notice(const char *title, const char *text)
{
    lv_obj_t *layer = lv_layer_top();

    /* 关键：让顶层图层参与命中测试，否则里面的按钮点不动（见文件头） */
    lv_obj_set_clickable(layer, true);
    if (!s_layer_hooked) {
        s_layer_hooked = true;
        lv_obj_add_event_cb(layer, layer_clicked_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *box = lv_msgbox_create(layer);
    lv_obj_set_width(box, 250); /* 固定宽度，避免长文本撑到圆角外 */
    lv_msgbox_add_title(box, title);
    lv_msgbox_add_text(box, text);
    /* footer 按钮自带 LVGL 内部的关闭回调（点击即关闭本对话框），无需再加 */
    lv_msgbox_add_footer_button(box, "确定");
    style_msgbox(box);
    lv_obj_center(box);

    /* 对话框删除后恢复图层可点击性 */
    lv_obj_add_event_cb(box, box_deleted_cb, LV_EVENT_DELETE, layer);
}

static void event_placeholder(lv_event_t *e)
{
    (void)e;
    show_notice("功能开发中", "该设置项将在后续版本提供");
}

static void event_about(lv_event_t *e)
{
    (void)e;
    char text[112];
    snprintf(text, sizeof(text), "XT60 功率计\n固件版本 v%s\n作者博客：blog.zeruns.com",
             esp_app_get_description()->version);
    show_notice("关于本机", text);
}

void screen_settings_create(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(tile, 16, 0); /* 避圆角 */
    lv_obj_set_style_pad_top(tile, 24, 0); /* 共享状态栏 */
    lv_obj_set_style_pad_bottom(tile, 14, 0);
    lv_obj_set_style_pad_row(tile, 2, 0);

    lv_obj_t *title = lv_label_create(tile);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_font(title, lvgl_port_get_font_bold(LVGL_PORT_FONT_20), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A1A1A), 0);

    /* 列表用「可滚动 flex 列 + 官方 lv_button」实现。
     * 不用 lv_list：它在 LVGL 9 已标记 deprecated（建议用 flex 列搭），
     * 而且自己搭能精确控制行高与中文字体，避免再踩默认字体的坑。 */
    lv_obj_t *list = lv_obj_create(tile);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(92));
    lv_obj_set_style_bg_color(list, lv_color_white(), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(0xE3E7EC), 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    struct {
        const char *icon;
        const char *text;
        lv_event_cb_t cb;
    } items[] = {
        { LV_SYMBOL_SETTINGS, "测量设置（标定/采样率）", event_placeholder },
        { LV_SYMBOL_IMAGE,    "显示设置（亮度/方向）",   event_placeholder },
        { LV_SYMBOL_AUDIO,    "蜂鸣器响度",              event_placeholder },
        { LV_SYMBOL_WIFI,     "WiFi 与热点",             event_placeholder },
        { LV_SYMBOL_REFRESH,  "电能清零与统计",          event_placeholder },
        { LV_SYMBOL_LIST,     "关于本机",                event_about },
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        lv_obj_t *btn = lv_button_create(list);
        lv_obj_set_size(btn, LV_PCT(100), 36);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xEDF0F4), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_hor(btn, 10, 0);
        lv_obj_set_style_pad_column(btn, 8, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(btn, false);
        lv_obj_add_event_cb(btn, items[i].cb, LV_EVENT_CLICKED, NULL);

        /* 图标：用中文主题字体的 fallback 链渲染 LV_SYMBOL_* */
        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, items[i].icon);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x0066CC), 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, items[i].text);
        lv_obj_set_style_text_font(lbl, lvgl_port_get_font(LVGL_PORT_FONT_16), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x1A1A1A), 0);
    }
}
