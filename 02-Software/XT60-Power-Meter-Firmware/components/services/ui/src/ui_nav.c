/**
 * @file ui_nav.c
 * @brief 三页导航实现：tileview 三页 + 固定共享状态栏 + 底部指示点 + 按键路由
 *
 * 层级（都在 screen 上，z 序按创建顺序）：
 *   tileview（三页内容，可横滑）
 *   → 共享状态栏（白底浮层，不拦截触摸，三页始终可见）
 *   → 底部页面指示点（不拦截触摸）
 *
 * 性能约定（重要）：数据刷新**只刷当前可见页**。
 *   之前两页都刷，波形页的 chart 每帧都要重算/重绘，占用大量 CPU 与
 *   LVGL 锁时间，导致测量页刷新率被拖慢（尤其选到 10min 时间窗时）。
 *   现在切页时强制刷一次新页，之后只刷当前页。
 *
 * 操作反馈：任何被接受的按键事件都短鸣一声（可在设置里关，见 buzzer_play）。
 */
#include "ui_nav.h"

#include <stdio.h>

#include "lvgl.h"
#include "lvgl_port.h"

#include "buzzer.h"
#include "screen_measure.h"
#include "screen_settings.h"
#include "screen_wave.h"

#define DOT_SIZE 6
#define DOT_W_CUR 16
#define DOT_H_CUR 6
#define DOT_GAP 7
#define DOT_Y 224
/** 状态栏内容左右内缩：面板顶部两角为圆角，文字必须离开圆角区域 */
#define TOPBAR_SAFE_X 28

/** 电池电量阈值（百分比）与对应颜色 */
#define BAT_COLOR_HIGH lv_color_hex(0x1E9E4F) /* ≥50 绿 */
#define BAT_COLOR_MID  lv_color_hex(0xE8A00C) /* 20~49 黄 */
#define BAT_COLOR_LOW  lv_color_hex(0xD0392B) /* 0~19 红 */
#define BAT_COLOR_NONE lv_color_hex(0xB9BFC7) /* 无数据 灰 */
#define BAT_FULL_MV 4200u
#define BAT_EMPTY_MV 3300u

static lv_obj_t *s_tileview = NULL;
static lv_obj_t *s_tiles[UI_PAGE_MAX];
static lv_obj_t *s_dots[UI_PAGE_MAX];
static lv_obj_t *s_lbl_runtime;
static lv_obj_t *s_lbl_bat;
static ui_page_t s_current = UI_PAGE_MEASURE;

static uint8_t battery_percent(uint32_t mv)
{
    if (mv == 0) {
        return 0;
    }
    if (mv <= BAT_EMPTY_MV) {
        return 0;
    }
    if (mv >= BAT_FULL_MV) {
        return 100;
    }
    return (uint8_t)((mv - BAT_EMPTY_MV) * 100u / (BAT_FULL_MV - BAT_EMPTY_MV));
}

static lv_color_t battery_color(uint8_t pct, bool has_data)
{
    if (!has_data) {
        return BAT_COLOR_NONE;
    }
    if (pct >= 50) {
        return BAT_COLOR_HIGH;
    }
    if (pct >= 20) {
        return BAT_COLOR_MID;
    }
    return BAT_COLOR_LOW;
}

/**
 * @brief 固定在屏幕顶部的共享状态栏（白底，触摸穿透，三页通用）
 */
static void create_topbar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 22);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_clickable(bar, false);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    /* 内缩 28px：这块区域正好是顶部两个圆角，文字进去会被遮住 */
    lv_obj_set_style_pad_hor(bar, TOPBAR_SAFE_X, 0);

    s_lbl_runtime = lv_label_create(bar);
    lv_label_set_text(s_lbl_runtime, "00:00:00");
    lv_obj_set_style_text_font(s_lbl_runtime, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(s_lbl_runtime, lv_color_hex(0x4A4F58), 0);
    lv_obj_set_clickable(s_lbl_runtime, false);

    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_clickable(right, false);
    lv_obj_set_scrollable(right, false);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 6, 0);

    /* WiFi/热点占位：wifi_manager 未接入，固定灰色，不伪造在线状态 */
    lv_obj_t *wifi = lv_label_create(right);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi, BAT_COLOR_NONE, 0);
    lv_obj_set_clickable(wifi, false);

    s_lbl_bat = lv_label_create(right);
    lv_label_set_text(s_lbl_bat, LV_SYMBOL_BATTERY_FULL " --%");
    lv_obj_set_style_text_font(s_lbl_bat, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(s_lbl_bat, BAT_COLOR_NONE, 0);
    lv_obj_set_clickable(s_lbl_bat, false);
}

static void refresh_status(const data_store_snapshot_t *snap)
{
    uint32_t total = snap->runtime_s;
    char rt[16];
    snprintf(rt, sizeof(rt), "%02u:%02u:%02u", (unsigned)(total / 3600),
             (unsigned)((total / 60) % 60), (unsigned)(total % 60));
    lv_label_set_text(s_lbl_runtime, rt);

    bool has = (snap->battery_mv > 0);
    uint8_t pct = battery_percent(snap->battery_mv);
    lv_color_t col = battery_color(pct, has);
    const char *icon = LV_SYMBOL_BATTERY_FULL;
    if (has) {
        if (pct < 20) {
            icon = LV_SYMBOL_BATTERY_EMPTY;
        } else if (pct < 40) {
            icon = LV_SYMBOL_BATTERY_1;
        } else if (pct < 60) {
            icon = LV_SYMBOL_BATTERY_2;
        } else if (pct < 80) {
            icon = LV_SYMBOL_BATTERY_3;
        }
    }
    char bat[24];
    if (has) {
        snprintf(bat, sizeof(bat), "%s %u%%", icon, (unsigned)pct);
    } else {
        snprintf(bat, sizeof(bat), "%s --%%", icon);
    }
    lv_label_set_text(s_lbl_bat, bat);
    lv_obj_set_style_text_color(s_lbl_bat, col, 0);
}

static void update_dots(void)
{
    for (int i = 0; i < UI_PAGE_MAX; i++) {
        bool cur = (i == (int)s_current);
        lv_obj_set_width(s_dots[i], cur ? DOT_W_CUR : DOT_SIZE);
        lv_obj_set_height(s_dots[i], cur ? DOT_H_CUR : DOT_SIZE);
        lv_obj_set_style_bg_color(s_dots[i],
                                  cur ? lv_color_hex(0x0066CC) : lv_color_hex(0xC2C7CF), 0);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
    }
}

static void scroll_event_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    int32_t w = lv_obj_get_width(tv);
    int32_t x = lv_obj_get_scroll_x(tv);
    if (w <= 0) {
        return;
    }
    int32_t idx = (x + w / 2) / w;
    if (idx < 0) {
        idx = 0;
    } else if (idx >= UI_PAGE_MAX) {
        idx = UI_PAGE_MAX - 1;
    }
    ui_page_t page = (ui_page_t)idx;
    if (page == s_current) {
        return;
    }
    s_current = page;
    update_dots();
}

static void goto_page(ui_page_t page)
{
    if (page < 0 || page >= UI_PAGE_MAX) {
        return;
    }
    lv_obj_scroll_to_view(s_tiles[page], LV_ANIM_ON);
}

void ui_nav_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);

    s_tileview = lv_tileview_create(scr);
    lv_obj_set_size(s_tileview, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_tileview, 0, 0);
    lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(s_tileview, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scroll_dir(s_tileview, LV_DIR_HOR);
    lv_obj_add_event_cb(s_tileview, scroll_event_cb, LV_EVENT_SCROLL, NULL);

    s_tiles[UI_PAGE_MEASURE] =
        lv_tileview_add_tile(s_tileview, UI_PAGE_MEASURE, 0, LV_DIR_HOR);
    s_tiles[UI_PAGE_WAVE] = lv_tileview_add_tile(s_tileview, UI_PAGE_WAVE, 0, LV_DIR_HOR);
    s_tiles[UI_PAGE_SETTINGS] =
        lv_tileview_add_tile(s_tileview, UI_PAGE_SETTINGS, 0, LV_DIR_HOR);

    screen_measure_create(s_tiles[UI_PAGE_MEASURE]);
    screen_wave_create(s_tiles[UI_PAGE_WAVE]);
    screen_settings_create(s_tiles[UI_PAGE_SETTINGS]);

    create_topbar(scr);

    lv_obj_t *dotbar = lv_obj_create(scr);
    lv_obj_remove_style_all(dotbar);
    lv_obj_set_size(dotbar, DOT_W_CUR + 2 * (DOT_SIZE + DOT_GAP), 12);
    lv_obj_set_pos(dotbar, (284 - (DOT_W_CUR + 2 * (DOT_SIZE + DOT_GAP))) / 2, DOT_Y);
    lv_obj_set_clickable(dotbar, false);
    lv_obj_set_scrollable(dotbar, false);
    lv_obj_set_flex_flow(dotbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dotbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dotbar, DOT_GAP, 0);

    for (int i = 0; i < UI_PAGE_MAX; i++) {
        s_dots[i] = lv_obj_create(dotbar);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_clickable(s_dots[i], false);
        lv_obj_set_scrollable(s_dots[i], false);
        lv_obj_set_size(s_dots[i], (i == UI_PAGE_MEASURE) ? DOT_W_CUR : DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
    }
    update_dots();

    s_current = (ui_page_t)0;
}

void ui_nav_refresh(const data_store_snapshot_t *snap)
{
    /* 状态栏（时间/电量）三页都要，必须每帧刷 */
    refresh_status(snap);

    /* 页面内容只刷当前可见页（见文件头"性能约定"） */
    switch (s_current) {
    case UI_PAGE_MEASURE:
        screen_measure_refresh(snap);
        break;
    case UI_PAGE_WAVE:
        screen_wave_refresh();
        break;
    case UI_PAGE_SETTINGS:
    default:
        break;
    }
}

void ui_nav_handle_key(button_id_t id, button_event_t ev)
{
    /* 被接受的按键都给一声短鸣（操作反馈） */
    bool handled = false;

    if (screen_wave_menu_is_open() && screen_wave_menu_key(id, ev)) {
        handled = true;
    } else if (ev == BUTTON_EVENT_SINGLE_CLICK) {
        if (id == BUTTON_ID_BOOT) {
            if (s_current > UI_PAGE_MEASURE) {
                goto_page((ui_page_t)(s_current - 1));
            }
        } else {
            if (s_current < UI_PAGE_SETTINGS) {
                goto_page((ui_page_t)(s_current + 1));
            }
        }
        handled = true;
    } else if (ev == BUTTON_EVENT_LONG_PRESS_START) {
        if (s_current == UI_PAGE_WAVE && id == BUTTON_ID_USER) {
            screen_wave_open_menu();
            handled = true;
        } else if (s_current == UI_PAGE_SETTINGS && id == BUTTON_ID_BOOT) {
            goto_page(UI_PAGE_MEASURE);
            handled = true;
        }
    }

    if (handled) {
        /* 单击与长按各给一次反馈；长按释放不重复响 */
        (void)buzzer_play(BUZZER_TONE_CLICK);
    }
}
