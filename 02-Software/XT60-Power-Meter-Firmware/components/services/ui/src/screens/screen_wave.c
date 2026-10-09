/**
 * @file screen_wave.c
 * @brief 波形页：双轴折线 + 轴刻度（带单位） + 时间轴 + 实时/平均数据行 + 设置滑板
 *
 * 版式（284×240，避圆角：内容左右 12px，顶部标题下移到 y=26）：
 *   ┌ x=12 ───────────────── x=272 ┐
 *   │        电压+电流 · 10s        │ y=26  标题（居中，天然避开圆角）
 *   │ ┌──────────────────────────┐ │ y=44
 *   │ │19.98 V             0.019 A│ │ 轴刻度带上单位（左主轴/右副轴）
 *   │ │         ╱╲  双轴折线      │ │ chart 高 138
 *   │ │19.90 V             0.018 A│ │
 *   │ └──────────────────────────┘ │ y=182
 *   │ -10s                    0s   │ 时间轴（相对"现在"）
 *   │ 实 19.97670V 0.01886A 0.37683W│ y=194
 *   │ 均 19.97612V 0.01880A 0.37540W│ y=207
 *   └───────────────────────────────┘
 *
 * 关键设计（针对实测问题）：
 *   · **右对齐滚动**：最新样本始终画在最右侧，历史向左推。以前把数据从左边
 *     开始填，选 10min 窗时「新点只占 1/6000 宽度」，看起来像"数据不动"；
 *   · **纵轴刻度带单位**，不再让用户猜"这条线是多少"；
 *   · **纵轴量程带滞回**：只有当数据超出当前量程、或缩小到不足一半才改量程，
 *     否则量程每帧抖动、曲线上下乱跳（实测 10s 窗量程在 15.6~-21.6 间乱变）；
 *   · 单位/时间/统计行都留了字号与间距，避免截断。
 */
#include "screen_wave.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "lvgl_port.h"

#define COL_V lv_color_hex(0x1E9E4F)
#define COL_I lv_color_hex(0x1B6FD6)
#define COL_P lv_color_hex(0xD0392B)
#define COL_E lv_color_hex(0x7D3CB5)
#define COL_DARK lv_color_hex(0x1A1A1A)
#define COL_GRAY lv_color_hex(0x6B7280)
#define COL_CARD_BD lv_color_hex(0xE3E7EC)
#define COL_PANEL_BG lv_color_hex(0xF7F9FC)

#define CHART_MAX_POINTS 240
#define CHART_VALUE_SCALE 100.0f
#define PANEL_H 168
#define SAFE_X 12
#define AXIS_LABEL_W 62

static lv_obj_t *s_chart = NULL;
static lv_chart_series_t *s_ser_a = NULL;
static lv_chart_series_t *s_ser_b = NULL;
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_time_l;
static lv_obj_t *s_lbl_time_r;
static lv_obj_t *s_axis_a[3];
static lv_obj_t *s_axis_b[3];
static lv_obj_t *s_lbl_realtime;
static lv_obj_t *s_lbl_average;

static pm_wave_window_t s_window = PM_WAVE_10S;
static pm_wave_mode_t s_mode = PM_WAVE_MODE_VI;

/* 纵轴量程滞回状态（避免每帧改量程导致曲线乱跳） */
static bool s_range_valid = false;
static float s_range_a_min, s_range_a_max, s_range_b_min, s_range_b_max;

static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_roller_win = NULL;
static lv_obj_t *s_roller_mode = NULL;
static int8_t s_focus_idx = 0;
static bool s_menu_open = false;
static bool s_panel_animating = false;

static float s_buf_a[CHART_MAX_POINTS];
static float s_buf_b[CHART_MAX_POINTS];

typedef struct {
    int8_t ch_a;
    int8_t ch_b;
    bool has_b;
    const char *unit_a;
    const char *unit_b;
} mode_desc_t;

static const mode_desc_t s_mode_desc[PM_WAVE_MODE_MAX] = {
    [PM_WAVE_MODE_P]  = { 2, -1, false, "W",  "" },
    [PM_WAVE_MODE_IP] = { 1, 2, true,  "A",  "W" },
    [PM_WAVE_MODE_VP] = { 0, 2, true,  "V",  "W" },
    [PM_WAVE_MODE_VI] = { 0, 1, true,  "V",  "A" },
    [PM_WAVE_MODE_E]  = { 3, -1, false, "Wh", "" },
};

static lv_color_t channel_color(int ch)
{
    switch (ch) {
    case 0: return COL_V;
    case 1: return COL_I;
    case 2: return COL_P;
    default: return COL_E;
    }
}

/** 按通道单位格式化轴值（带单位后缀） */
static void format_axis(char *dst, size_t n, int ch, float v)
{
    const char *u = "";
    const char *fmt;
    switch (ch) {
    case 0: fmt = "%.2f"; u = "V"; break;
    case 1: fmt = "%.3f"; u = "A"; break;
    case 2: fmt = "%.1f"; u = "W"; break;
    default: fmt = "%.3f"; u = "Wh"; break;
    }
    snprintf(dst, n, fmt, (double)v);
    size_t len = strlen(dst);
    snprintf(dst + len, n - len, " %s", u);
}

static void update_focus_style(void)
{
    lv_obj_t *targets[2] = { s_roller_win, s_roller_mode };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_border_color(targets[i],
                                     (i == s_focus_idx) ? lv_color_hex(0x0066CC) : COL_CARD_BD,
                                     0);
        lv_obj_set_style_border_width(targets[i], (i == s_focus_idx) ? 2 : 1, 0);
    }
}

static void apply_roller_selection(void)
{
    int w = lv_roller_get_selected(s_roller_win);
    int m = lv_roller_get_selected(s_roller_mode);
    bool changed = false;
    if (w >= 0 && w < PM_WAVE_WINDOW_MAX && (pm_wave_window_t)w != s_window) {
        s_window = (pm_wave_window_t)w;
        changed = true;
    }
    if (m >= 0 && m < PM_WAVE_MODE_MAX && (pm_wave_mode_t)m != s_mode) {
        s_mode = (pm_wave_mode_t)m;
        changed = true;
    }
    if (changed) {
        s_range_valid = false; /* 换了窗/模式，量程重新自适应 */
    }
}

static void update_header(void)
{
    char t[48];
    snprintf(t, sizeof(t), "%s · %s", pm_wave_mode_name(s_mode), pm_wave_window_name(s_window));
    lv_label_set_text(s_lbl_title, t);

    /* 时间轴：左端 -窗长，右端 0（现在） */
    char tl[16];
    snprintf(tl, sizeof(tl), "-%s", pm_wave_window_name(s_window));
    lv_label_set_text(s_lbl_time_l, tl);
    lv_label_set_text(s_lbl_time_r, "0");

    const mode_desc_t *d = &s_mode_desc[s_mode];
    lv_chart_set_series_color(s_chart, s_ser_a, channel_color(d->ch_a));
    lv_chart_hide_series(s_chart, s_ser_b, !d->has_b);
    if (d->has_b) {
        lv_chart_set_series_color(s_chart, s_ser_b, channel_color(d->ch_b));
    }
    for (int k = 0; k < 3; k++) {
        lv_obj_set_style_text_color(s_axis_a[k], channel_color(d->ch_a), 0);
        lv_obj_set_style_text_color(s_axis_b[k], channel_color(d->ch_b), 0);
        if (d->has_b) {
            lv_obj_set_hidden(s_axis_b[k], false);
        } else {
            lv_obj_set_hidden(s_axis_b[k], true);
        }
    }
}

/* ---- 滑板动画 ---- */
static void anim_panel_y(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void panel_open_done(lv_anim_t *a)
{
    (void)a;
    s_panel_animating = false;
    s_menu_open = true;
}

static void panel_close_done(lv_anim_t *a)
{
    (void)a;
    lv_obj_set_hidden(s_panel, true);
    s_panel_animating = false;
    s_menu_open = false;
}

void screen_wave_open_menu(void)
{
    if (s_panel == NULL || s_menu_open || s_panel_animating) {
        return;
    }
    lv_obj_set_hidden(s_panel, false);
    s_panel_animating = true;
    update_focus_style();

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_panel);
    lv_anim_set_values(&anim, 240, 240 - PANEL_H);
    lv_anim_set_duration(&anim, 220);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, anim_panel_y);
    lv_anim_set_completed_cb(&anim, panel_open_done);
    lv_anim_start(&anim);
}

void screen_wave_close_menu(bool apply)
{
    if (s_panel == NULL || (!s_menu_open && !s_panel_animating)) {
        s_menu_open = false;
        s_panel_animating = false;
        if (s_panel != NULL) {
            lv_obj_set_hidden(s_panel, true);
        }
        return;
    }
    if (s_menu_open && apply) {
        apply_roller_selection();
        update_header();
    }
    s_menu_open = false;
    s_panel_animating = true;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_panel);
    lv_anim_set_values(&anim, 240 - PANEL_H, 240);
    lv_anim_set_duration(&anim, 180);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&anim, anim_panel_y);
    lv_anim_set_completed_cb(&anim, panel_close_done);
    lv_anim_start(&anim);
}

bool screen_wave_menu_is_open(void)
{
    return s_menu_open;
}

bool screen_wave_menu_key(button_id_t id, button_event_t ev)
{
    if (!s_menu_open) {
        return false;
    }

    switch (ev) {
    case BUTTON_EVENT_SINGLE_CLICK: {
        lv_obj_t *roller = (s_focus_idx == 0) ? s_roller_win : s_roller_mode;
        int cnt = (s_focus_idx == 0) ? PM_WAVE_WINDOW_MAX : PM_WAVE_MODE_MAX;
        int sel = lv_roller_get_selected(roller);
        if (id == BUTTON_ID_BOOT) {
            sel = (sel - 1 + cnt) % cnt;
        } else {
            sel = (sel + 1) % cnt;
        }
        lv_roller_set_selected(roller, (uint16_t)sel, LV_ANIM_ON);
        return true;
    }
    case BUTTON_EVENT_DOUBLE_CLICK:
        if (id == BUTTON_ID_USER) {
            s_focus_idx = (int8_t)((s_focus_idx + 1) % 2);
            update_focus_style();
        }
        return true;
    case BUTTON_EVENT_LONG_PRESS_START:
        screen_wave_close_menu(id == BUTTON_ID_USER);
        return true;
    default:
        return true;
    }
}

static void gesture_event_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_TOP) {
        screen_wave_open_menu();
    } else if (dir == LV_DIR_BOTTOM) {
        screen_wave_close_menu(true);
    }
}

/**
 * @brief 纵轴量程计算（带滞回，避免曲线每帧乱跳）
 *
 * 规则：数据超出当前量程 → 按新数据扩量程；
 *       数据跨度不足当前量程的 40% → 收缩（滞后系数防止反复）；
 *       其余情况保持不变。
 */
static void update_axis_range(void)
{
    const mode_desc_t *d = &s_mode_desc[s_mode];
    float a_min, a_max, b_min, b_max;
    bool has_b = pm_wave_get_range(s_window, s_mode, &a_min, &a_max, &b_min, &b_max);

    if (!s_range_valid) {
        s_range_a_min = a_min;
        s_range_a_max = a_max;
        s_range_b_min = b_min;
        s_range_b_max = b_max;
        s_range_valid = true;
    } else {
        bool outside = (a_min < s_range_a_min) || (a_max > s_range_a_max);
        float cur_span = s_range_a_max - s_range_a_min;
        if (outside || (cur_span > 1e-9f && (a_max - a_min) < cur_span * 0.4f)) {
            s_range_a_min = a_min;
            s_range_a_max = a_max;
        }
        if (has_b) {
            bool outside_b = (b_min < s_range_b_min) || (b_max > s_range_b_max);
            float cur_span_b = s_range_b_max - s_range_b_min;
            if (outside_b || (cur_span_b > 1e-9f && (b_max - b_min) < cur_span_b * 0.4f)) {
                s_range_b_min = b_min;
                s_range_b_max = b_max;
            }
        }
    }

    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y,
                            (int32_t)(s_range_a_min * CHART_VALUE_SCALE),
                            (int32_t)(s_range_a_max * CHART_VALUE_SCALE));
    if (d->has_b) {
        lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_SECONDARY_Y,
                                (int32_t)(s_range_b_min * CHART_VALUE_SCALE),
                                (int32_t)(s_range_b_max * CHART_VALUE_SCALE));
    }

    /* 轴刻度（上/中/下） */
    char txt[16];
    const float avals[3] = { s_range_a_max, (s_range_a_min + s_range_a_max) * 0.5f,
                             s_range_a_min };
    const float bvals[3] = { s_range_b_max, (s_range_b_min + s_range_b_max) * 0.5f,
                             s_range_b_min };
    for (int k = 0; k < 3; k++) {
        format_axis(txt, sizeof(txt), d->ch_a, avals[k]);
        lv_label_set_text(s_axis_a[k], txt);
        if (d->has_b) {
            format_axis(txt, sizeof(txt), d->ch_b, bvals[k]);
            lv_label_set_text(s_axis_b[k], txt);
        }
    }
}

void screen_wave_refresh(void)
{
    if (s_chart == NULL) {
        return;
    }

    int n = pm_wave_copy(s_window, s_mode, s_buf_a, s_buf_b, CHART_MAX_POINTS);
    const mode_desc_t *d = &s_mode_desc[s_mode];

    update_axis_range();

    /* 右对齐滚动：把 n 个点画在序列末尾，前面填 NONE。
     * 这样最新样本永远贴右边缘，时间推进时整体向左走（见文件头说明）。 */
    int base = CHART_MAX_POINTS - n;
    if (base < 0) {
        base = 0;
    }
    for (int k = 0; k < CHART_MAX_POINTS; k++) {
        int src = k - base;
        if (src >= 0 && src < n) {
            lv_chart_set_series_value_by_id(s_chart, s_ser_a, (uint32_t)k,
                                           (int32_t)(s_buf_a[src] * CHART_VALUE_SCALE));
            if (d->has_b) {
                lv_chart_set_series_value_by_id(s_chart, s_ser_b, (uint32_t)k,
                                               (int32_t)(s_buf_b[src] * CHART_VALUE_SCALE));
            }
        } else {
            lv_chart_set_series_value_by_id(s_chart, s_ser_a, (uint32_t)k, LV_CHART_POINT_NONE);
            if (d->has_b) {
                lv_chart_set_series_value_by_id(s_chart, s_ser_b, (uint32_t)k,
                                               LV_CHART_POINT_NONE);
            }
        }
    }
    lv_chart_refresh(s_chart);

    /* 底部实时/平均数据行：按当前模式给出两条线对应的量与功率 */
    if (n <= 0) {
        return;
    }
    float a_last = s_buf_a[n - 1];
    float b_last = d->has_b ? s_buf_b[n - 1] : 0.0f;
    float a_sum = 0.0f, b_sum = 0.0f;
    for (int k = 0; k < n; k++) {
        a_sum += s_buf_a[k];
        if (d->has_b) {
            b_sum += s_buf_b[k];
        }
    }
    float a_avg = a_sum / (float)n;
    float b_avg = d->has_b ? (b_sum / (float)n) : 0.0f;

    char rt[72], av[72];
    switch (s_mode) {
    case PM_WAVE_MODE_VI:
        snprintf(rt, sizeof(rt), "实 %.5fV %.5fA %.5fW", (double)a_last, (double)b_last,
                 (double)(a_last * b_last));
        snprintf(av, sizeof(av), "均 %.5fV %.5fA %.5fW", (double)a_avg, (double)b_avg,
                 (double)(a_avg * b_avg));
        break;
    case PM_WAVE_MODE_VP:
        snprintf(rt, sizeof(rt), "实 %.5fV %.5fW", (double)a_last, (double)b_last);
        snprintf(av, sizeof(av), "均 %.5fV %.5fW", (double)a_avg, (double)b_avg);
        break;
    case PM_WAVE_MODE_IP:
        snprintf(rt, sizeof(rt), "实 %.5fA %.5fW", (double)a_last, (double)b_last);
        snprintf(av, sizeof(av), "均 %.5fA %.5fW", (double)a_avg, (double)b_avg);
        break;
    case PM_WAVE_MODE_P:
        snprintf(rt, sizeof(rt), "实 %.5fW", (double)a_last);
        snprintf(av, sizeof(av), "均 %.5fW", (double)a_avg);
        break;
    default: /* 电能 */
        snprintf(rt, sizeof(rt), "实 %.5fWh", (double)a_last);
        snprintf(av, sizeof(av), "均 %.5fWh", (double)a_avg);
        break;
    }
    lv_label_set_text(s_lbl_realtime, rt);
    lv_label_set_text(s_lbl_average, av);
}

/**
 * @brief chart 尺寸确定后摆放 6 个轴刻度（左/右 padding 留白区内，上中下）
 * @note 子对象坐标相对 chart 的 content area，故用负坐标放进左侧 padding
 */
static void chart_size_event_cb(lv_event_t *e)
{
    lv_obj_t *chart = lv_event_get_current_target(e);
    lv_coord_t w = lv_obj_get_content_width(chart);
    lv_coord_t h = lv_obj_get_content_height(chart);
    for (int k = 0; k < 3; k++) {
        lv_coord_t y = (k == 0) ? -3 : (k == 1) ? (h - 14) / 2 : (h - 11);
        lv_obj_set_pos(s_axis_a[k], -(AXIS_LABEL_W + 4), y);
        lv_obj_set_pos(s_axis_b[k], w + 4, y);
    }
}

static void create_panel(lv_obj_t *scr)
{
    s_panel = lv_obj_create(scr);
    lv_obj_set_size(s_panel, LV_PCT(100), PANEL_H);
    lv_obj_set_pos(s_panel, 0, 240);
    lv_obj_set_style_bg_color(s_panel, COL_PANEL_BG, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 12, 0);
    lv_obj_set_style_border_color(s_panel, COL_CARD_BD, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_pad_all(s_panel, 10, 0);
    lv_obj_set_style_pad_hor(s_panel, 16, 0); /* 面板内也避开圆角 */
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 4, 0);
    lv_obj_set_hidden(s_panel, true);
    lv_obj_set_scrollable(s_panel, false);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "波形设置");
    lv_obj_set_style_text_font(title, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    lv_obj_set_style_text_color(title, COL_DARK, 0);

    lv_obj_t *row = lv_obj_create(s_panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 88);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *col_w = lv_obj_create(row);
    lv_obj_remove_style_all(col_w);
    lv_obj_set_size(col_w, 116, 88);
    lv_obj_set_scrollable(col_w, false);
    lv_obj_set_flex_flow(col_w, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_w, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col_w, 2, 0);
    lv_obj_t *lw = lv_label_create(col_w);
    lv_label_set_text(lw, "时间窗");
    lv_obj_set_style_text_font(lw, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(lw, COL_GRAY, 0);
    s_roller_win = lv_roller_create(col_w);
    lv_roller_set_options(s_roller_win, "1s\n10s\n30s\n60s\n120s\n10min",
                          LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_win, 3);
    lv_roller_set_selected(s_roller_win, (uint16_t)s_window, LV_ANIM_OFF);
    lv_obj_set_size(s_roller_win, 108, 68);

    lv_obj_t *col_m = lv_obj_create(row);
    lv_obj_remove_style_all(col_m);
    lv_obj_set_size(col_m, 146, 88);
    lv_obj_set_scrollable(col_m, false);
    lv_obj_set_flex_flow(col_m, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_m, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col_m, 2, 0);
    lv_obj_t *lm = lv_label_create(col_m);
    lv_label_set_text(lm, "波形模式");
    lv_obj_set_style_text_font(lm, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(lm, COL_GRAY, 0);
    s_roller_mode = lv_roller_create(col_m);
    lv_roller_set_options(s_roller_mode,
                          "功率\n电流+功率\n电压+功率\n电压+电流\n电能", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_mode, 3);
    lv_roller_set_selected(s_roller_mode, (uint16_t)s_mode, LV_ANIM_OFF);
    lv_obj_set_size(s_roller_mode, 138, 68);

    lv_obj_t *hint = lv_label_create(s_panel);
    lv_label_set_text(hint, "K1/K2 上下 · K2 双击切项 · K2 长按确定 · K1 长按退出");
    lv_obj_set_style_text_font(hint, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(hint, COL_GRAY, 0);
}

void screen_wave_create(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_add_event_cb(tile, gesture_event_cb, LV_EVENT_GESTURE, NULL);

    /* 标题（居中，天然避开圆角） */
    s_lbl_title = lv_label_create(tile);
    lv_label_set_text(s_lbl_title, "");
    lv_obj_set_style_text_font(s_lbl_title, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(s_lbl_title, COL_DARK, 0);
    lv_obj_set_pos(s_lbl_title, 0, 26);
    lv_obj_set_width(s_lbl_title, 284);
    lv_obj_set_style_text_align(s_lbl_title, LV_TEXT_ALIGN_CENTER, 0);

    /* 图表 */
    s_chart = lv_chart_create(tile);
    lv_obj_set_size(s_chart, 284 - 2 * SAFE_X, 138);
    lv_obj_set_pos(s_chart, SAFE_X, 44);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_MAX_POINTS);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_chart, 4, 5);
    lv_obj_set_style_pad_left(s_chart, AXIS_LABEL_W + 4, 0);  /* 给左轴刻度留位 */
    lv_obj_set_style_pad_right(s_chart, AXIS_LABEL_W + 4, 0); /* 给右轴刻度留位 */
    lv_obj_set_style_pad_top(s_chart, 5, 0);
    lv_obj_set_style_pad_bottom(s_chart, 5, 0);
    lv_obj_set_style_bg_color(s_chart, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_chart, COL_CARD_BD, 0);
    lv_obj_set_style_border_width(s_chart, 1, 0);
    lv_obj_set_style_radius(s_chart, 8, 0);
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_MAIN);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_chart, chart_size_event_cb, LV_EVENT_SIZE_CHANGED, NULL);

    s_ser_a = lv_chart_add_series(s_chart, COL_V, LV_CHART_AXIS_PRIMARY_Y);
    s_ser_b = lv_chart_add_series(s_chart, COL_I, LV_CHART_AXIS_SECONDARY_Y);
    lv_chart_set_all_values(s_chart, s_ser_a, LV_CHART_POINT_NONE);
    lv_chart_set_all_values(s_chart, s_ser_b, LV_CHART_POINT_NONE);

    /* 轴刻度（上/中/下 × 左/右），放在 chart 的 padding 留白区内 */
    const lv_font_t *f12 = lvgl_port_get_font(LVGL_PORT_FONT_12);
    for (int k = 0; k < 3; k++) {
        s_axis_a[k] = lv_label_create(s_chart);
        lv_obj_set_size(s_axis_a[k], AXIS_LABEL_W, 14);
        lv_obj_set_style_text_font(s_axis_a[k], f12, 0);
        lv_obj_set_style_pad_hor(s_axis_a[k], 2, 0);
        lv_label_set_text(s_axis_a[k], "--");
        lv_obj_set_clickable(s_axis_a[k], false);
        lv_obj_set_scrollable(s_axis_a[k], false);

        s_axis_b[k] = lv_label_create(s_chart);
        lv_obj_set_size(s_axis_b[k], AXIS_LABEL_W, 14);
        lv_obj_set_style_text_font(s_axis_b[k], f12, 0);
        lv_obj_set_style_pad_hor(s_axis_b[k], 2, 0);
        lv_obj_set_style_text_align(s_axis_b[k], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(s_axis_b[k], "--");
        lv_obj_set_clickable(s_axis_b[k], false);
        lv_obj_set_scrollable(s_axis_b[k], false);
    }

    /* 时间轴（左：-窗长，右：0） */
    s_lbl_time_l = lv_label_create(tile);
    lv_label_set_text(s_lbl_time_l, "");
    lv_obj_set_style_text_font(s_lbl_time_l, f12, 0);
    lv_obj_set_style_text_color(s_lbl_time_l, COL_GRAY, 0);
    lv_obj_set_pos(s_lbl_time_l, SAFE_X + AXIS_LABEL_W + 4, 184);

    s_lbl_time_r = lv_label_create(tile);
    lv_label_set_text(s_lbl_time_r, "0");
    lv_obj_set_style_text_font(s_lbl_time_r, f12, 0);
    lv_obj_set_style_text_color(s_lbl_time_r, COL_GRAY, 0);
    lv_obj_set_pos(s_lbl_time_r, 284 - SAFE_X - AXIS_LABEL_W - 4, 184);

    /* 实时 / 平均 */
    s_lbl_realtime = lv_label_create(tile);
    lv_label_set_text(s_lbl_realtime, "实 --");
    lv_obj_set_style_text_font(s_lbl_realtime, f12, 0);
    lv_obj_set_style_text_color(s_lbl_realtime, COL_DARK, 0);
    lv_obj_set_pos(s_lbl_realtime, SAFE_X, 196);

    s_lbl_average = lv_label_create(tile);
    lv_label_set_text(s_lbl_average, "均 --");
    lv_obj_set_style_text_font(s_lbl_average, f12, 0);
    lv_obj_set_style_text_color(s_lbl_average, COL_GRAY, 0);
    lv_obj_set_pos(s_lbl_average, SAFE_X, 209);

    update_header();
    create_panel(lv_screen_active());
}

pm_wave_window_t screen_wave_get_window(void)
{
    return s_window;
}

pm_wave_mode_t screen_wave_get_mode(void)
{
    return s_mode;
}
