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
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "lvgl_port.h"
#include "settings.h"

/* 波形区黑底配色（示波器惯例：黑底 + 亮线，对比度高、聚焦曲线）。
 * 2026-10 波形重构：白底上灰格线+多色曲线视觉杂乱（视频实证"一片乱"），
 * 改黑底亮线后曲线成为绝对主体——Tektronix/Keysight/DSO Nano 均为此风格。 */
#define COL_WAVE_BG   lv_color_hex(0x0A0E14) /* 波形区深黑底 */
#define COL_WAVE_GRID lv_color_hex(0x2B333B) /* 格线暗灰（不抢眼） */
#define COL_V lv_color_hex(0x3FB950) /* CH1 电压：亮绿 */
#define COL_I lv_color_hex(0x58A6FF) /* CH2 电流：亮蓝 */
#define COL_P lv_color_hex(0xF0883E) /* CH3 功率：亮橙 */
#define COL_E lv_color_hex(0xA371F7) /* CH4 电能：亮紫 */
#define COL_DARK lv_color_hex(0x1A1A1A)
#define COL_GRAY lv_color_hex(0x6B7280)
#define COL_CARD_BD lv_color_hex(0xB4BEC9) /* 边框：与白底拉开可辨差异 */
#define COL_PANEL_BG lv_color_hex(0xF1F4F8)
#define COL_PRIMARY lv_color_hex(0x0066CC) /* 品牌蓝（与设置页一致，用于开关开启态） */

#define CHART_MAX_POINTS 480
#define CHART_VALUE_SCALE 100.0f
/* 滑板高度：title 20 + row 88 + 平滑行 28+4 + 提示两行 32 + 行距 4×4 +
 * 内边距 20 ≈ 224。旧值 188 是加"曲线平滑"开关行之前的布局。 */
#define PANEL_H 224
#define SAFE_X 12
#define AXIS_LABEL_W 62

/**
 * @brief 波形页布局几何（横/竖屏各一套；旋转重建时重新取值）
 *
 * 横屏 284×240 为历史调优值（逐像素保持）；竖屏 240×284 时图表加高
 * 吃满多出的垂直空间，时间轴/数据行随之下移，设置滑板的两列 roller
 * 收窄以适配 240 屏宽。
 */
typedef struct {
    int32_t w;          /**< 逻辑宽 */
    int32_t h;          /**< 逻辑高 */
    int32_t chart_y;    /**< 图表顶部 y */
    int32_t chart_h;    /**< 图表高 */
    int32_t time_y;     /**< 时间轴 y */
    int32_t rt_y;       /**< 实时行 y */
    int32_t av_y;       /**< 平均行 y */
    int32_t col_win_w;  /**< 滑板"时间窗"列宽 */
    int32_t col_mode_w; /**< 滑板"波形模式"列宽 */
} wave_geom_t;

static wave_geom_t wave_geom(void)
{
    lv_display_t *d = lv_display_get_default();
    int32_t w = (d != NULL) ? lv_display_get_horizontal_resolution(d) : 284;
    int32_t h = (d != NULL) ? lv_display_get_vertical_resolution(d) : 240;
    wave_geom_t g;
    g.w = w;
    g.h = h;
    if (w >= h) {
        /* 横屏 284×240 */
        g.chart_y = 44;
        g.chart_h = 138;
        g.time_y = 184;
        g.rt_y = 196;
        g.av_y = 209;
        g.col_win_w = 116;
        g.col_mode_w = 146;
    } else {
        /* 竖屏 240×284：图表加高，时间轴/数据行整体下移 */
        g.chart_y = 44;
        g.chart_h = 170;
        g.time_y = 220;
        g.rt_y = 232;
        g.av_y = 245;
        g.col_win_w = 96;
        g.col_mode_w = 104;
    }
    return g;
}

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
/** 量程收缩防抖：连续满足收缩条件的帧数（阈值见 RANGE_SHRINK_FRAMES） */
static int s_shrink_cnt_a = 0;
static int s_shrink_cnt_b = 0;
/** 极值点防跳动锚点（NAN 表示尚未锚定） */
static float s_hold_min_val = NAN;
static float s_hold_max_val = NAN;

static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_roller_win = NULL;
static lv_obj_t *s_roller_mode = NULL;
static int8_t s_focus_idx = 0;
static bool s_menu_open = false;
static bool s_panel_animating = false;
/** roller 触控两阶段状态：-1=无（不可滑动）/0=时间窗选中/1=波形模式选中。
 *  点击选中后该 roller 才允许手指滑动；点另一个或关菜单即取消。
 *  （上一版直接 set_clickable(false) 完全禁触控过头了——用户要的是
 *   「点击选中后才能滑动」，既防下滑菜单时误改，又保留触控能力。） */
static int8_t s_roller_edit = -1;

static float s_buf_a[CHART_MAX_POINTS];
static float s_buf_b[CHART_MAX_POINTS];
static float s_buf_a_raw[CHART_MAX_POINTS]; /**< 平滑前原值（平滑开时暂存） */
static float s_buf_b_raw[CHART_MAX_POINTS]; /**< 副轴平滑前原值（Step2 降噪用） */

/* ---- 趋势图装饰（P0）：均值参考线 + min/max 极值点 ----
 * 【波形重构 Step1·做减法】视频实证波形"一片乱"的主因之一是装饰元素过多。
 * 调研结论（Tektronix/DSO Nano/仪表惯例）：小屏波形应只保留曲线+格线+少量
 * 刻度，均值线/min-max 圆点全删。默认关闭，若需要趋势标注可改回 1。
 * 用 lv_obj 而非 chart series：样式独立（1px 细线 / 小圆点），
 * 不污染曲线本身的线宽/点样式。挂 chart 下，坐标相对 content area。 */
#define WAVE_DECOR_ENABLE 0
static lv_obj_t *s_avg_line = NULL;
static lv_obj_t *s_dot_min = NULL;
static lv_obj_t *s_dot_max = NULL;
static lv_obj_t *s_lbl_time_m = NULL; /**< 时间轴中点刻度 */

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

/** roller 触控两阶段：点击选中后才允许滑动（防下滑关菜单时误改） */
static void roller_clicked_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    int8_t idx = (obj == s_roller_win) ? 0 : 1;
    if (s_roller_edit == idx) {
        /* 已在编辑态：本次点击视为"确认当前选择"，退出编辑态 */
        s_roller_edit = -1;
        lv_obj_set_scrollable(s_roller_win, false);
        lv_obj_set_scrollable(s_roller_mode, false);
    } else {
        /* 进入编辑态：只允许选中的 roller 滑动 */
        s_roller_edit = idx;
        lv_obj_set_scrollable(s_roller_win, idx == 0);
        lv_obj_set_scrollable(s_roller_mode, idx == 1);
        s_focus_idx = idx;
        update_focus_style();
    }
}

/** 关闭 roller 编辑态（关菜单/长按确认等调用） */
static void roller_edit_cancel(void)
{
    s_roller_edit = -1;
    if (s_roller_win != NULL) {
        lv_obj_set_scrollable(s_roller_win, false);
    }
    if (s_roller_mode != NULL) {
        lv_obj_set_scrollable(s_roller_mode, false);
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
        s_shrink_cnt_a = 0;
        s_shrink_cnt_b = 0;
        s_hold_min_val = NAN; /* 极值锚点失效，重新锚定 */
        s_hold_max_val = NAN;
    }
}

static void update_header(void)
{
    char t[48];
    snprintf(t, sizeof(t), "%s · %s", pm_wave_mode_name(s_mode), pm_wave_window_name(s_window));
    lv_label_set_text(s_lbl_title, t);

    /* 时间轴：左 -窗长、中 -半窗、右 0（贴图面左右缘） */
    char tl[16];
    snprintf(tl, sizeof(tl), "-%s", pm_wave_window_name(s_window));
    lv_label_set_text(s_lbl_time_l, tl);
    lv_label_set_text(s_lbl_time_r, "0");
    if (s_lbl_time_m != NULL) {
        static const char *const half_name[PM_WAVE_WINDOW_MAX] = {
            "0.5s", "5s", "15s", "30s", "60s", "5min",
        };
        char tm[16];
        snprintf(tm, sizeof(tm), "-%s", half_name[s_window % PM_WAVE_WINDOW_MAX]);
        lv_label_set_text(s_lbl_time_m, tm);
    }

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
    wave_geom_t g = wave_geom();
    lv_obj_set_hidden(s_panel, false);
    s_panel_animating = true;
    update_focus_style();

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_panel);
    lv_anim_set_values(&anim, g.h, g.h - PANEL_H); /* 自底部升起（横240/竖284） */
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
    roller_edit_cancel(); /* 关菜单时退出编辑态 */
    s_menu_open = false;
    s_panel_animating = true;

    wave_geom_t g = wave_geom();
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_panel);
    lv_anim_set_values(&anim, g.h - PANEL_H, g.h); /* 沉回屏外（横240/竖284） */
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

/** 波形页滑动：上滑出设置滑板 / 下滑关闭（由 ui_nav indev 级滑动识别调用） */
void screen_wave_swipe_cb(lv_dir_t dir)
{
    if (dir == LV_DIR_TOP) {
        screen_wave_open_menu();
    } else if (dir == LV_DIR_BOTTOM) {
        screen_wave_close_menu(true);
    }
}

/**
 * @brief 稳健量程：取窗口内 2%~98% 分位，剔除 ADC 异常尖峰
 *
 * 2026-10 实拍（VID_160937 f03）：ADC 坏帧尖峰把电流量程拉到
 * -13~+18A，而真实信号仅 ±0.1A——趋势被压成中间一条细线。
 * 分位截断后再做滞回，趋势才可读。
 *
 * 性能：只需求第 i0/i1 个顺序统计量，不必全排序。原实现每帧 qsort
 * 240 个 float（O(n log n) 且写快排常数），改为两次快速选择 O(n)。
 */
/** 快速选择：把第 k 小的元素放到 a[k]，左侧 ≤、右侧 ≥（原地，不保证整体有序） */
static void select_kth(float *a, int n, int k)
{
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = a[lo + (hi - lo) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) {
                i++;
            }
            while (a[j] > pivot) {
                j--;
            }
            if (i <= j) {
                float t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        if (k <= j) {
            hi = j;
        } else if (k >= i) {
            lo = i;
        } else {
            return; /* pivot 就位 */
        }
    }
}

static bool robust_range(const float *buf, int n, float *lo, float *hi)
{
    if (buf == NULL || n <= 0) {
        return false;
    }
    float tmp[CHART_MAX_POINTS];
    int m = 0;
    for (int i = 0; i < n && m < CHART_MAX_POINTS; i++) {
        if (isfinite(buf[i])) {
            tmp[m++] = buf[i];
        }
    }
    if (m == 0) {
        return false;
    }
    int i0 = (int)((float)(m - 1) * 0.02f);
    int i1 = (int)((float)(m - 1) * 0.98f);
    if (i1 <= i0) {
        i0 = 0;
        i1 = m - 1;
    }
    /* 两次快速选择必须**分区间**做，否则后者会挪动前者的结果：
     *   ① 先在整个 [0,m) 取第 i1 小 → tmp[i1] 就位，前缀 [0,i1) ≤ 它；
     *   ② 再只在 [0,i1) 取第 i0 小 → tmp[i0] 就位，且不触碰 tmp[i1]。
     * 第 i0 小必在前缀内（i0 < i1，排序位置 i0 落在第 0..i1-1 名），故②正确。 */
    select_kth(tmp, m, i1);
    if (i0 < i1) {
        select_kth(tmp, i1, i0);
    }
    *lo = tmp[i0];
    *hi = tmp[i1];
    if (*hi - *lo < 1e-6f) {
        /* 信号几乎恒定：给一点最小跨度，避免除零/全平线 */
        float mid = *lo;
        float pad = fabsf(mid) * 0.01f + 1e-4f;
        *lo = mid - pad;
        *hi = mid + pad;
    }
    return true;
}

/**
 * @brief 纵轴量程计算（稳健分位 + 滞回 + 收缩防抖，避免每帧改量程导致曲线乱跳）
 *
 * 规则：先取窗口 2%~98% 分位（剔尖峰）；数据超出当前量程 → 扩量程（立即）；
 *       数据跨度不足当前量程的 40% → 收缩，但需**连续 N 帧**满足才执行
 *       （防抖：单帧的收缩误判会让量程在两个值间反复横跳）；
 *       其余情况保持不变。
 */

/** 收缩防抖：连续满足收缩条件的帧数阈值（实测 3~5 帧较稳） */
#define RANGE_SHRINK_FRAMES 4

static void update_axis_range(int n)
{
    const mode_desc_t *d = &s_mode_desc[s_mode];
    float a_min, a_max, b_min, b_max;
    if (!robust_range(s_buf_a_raw, n, &a_min, &a_max)) {
        return;
    }
    bool has_b = d->has_b && robust_range(s_buf_b, n, &b_min, &b_max);

    if (!s_range_valid) {
        s_range_a_min = a_min;
        s_range_a_max = a_max;
        s_range_b_min = b_min;
        s_range_b_max = b_max;
        s_range_valid = true;
        s_shrink_cnt_a = 0;
        s_shrink_cnt_b = 0;
    } else {
        /* a 通道：扩程加滞回余量（数据超出当前量程 3% 才扩，防单点噪声
         * 触发扩程跳动）；缩程需连续帧确认（防抖） */
        bool outside = (a_min < s_range_a_min - (s_range_a_max - s_range_a_min) * 0.03f) ||
                       (a_max > s_range_a_max + (s_range_a_max - s_range_a_min) * 0.03f);
        float cur_span = s_range_a_max - s_range_a_min;
        bool want_shrink = (cur_span > 1e-9f && (a_max - a_min) < cur_span * 0.4f);
        if (outside) {
            s_range_a_min = a_min;
            s_range_a_max = a_max;
            s_shrink_cnt_a = 0;
        } else if (want_shrink) {
            s_shrink_cnt_a++;
            if (s_shrink_cnt_a >= RANGE_SHRINK_FRAMES) {
                s_range_a_min = a_min;
                s_range_a_max = a_max;
                s_shrink_cnt_a = 0;
            }
        } else {
            s_shrink_cnt_a = 0;
        }
        if (has_b) {
            bool outside_b = (b_min < s_range_b_min - (s_range_b_max - s_range_b_min) * 0.03f) ||
                             (b_max > s_range_b_max + (s_range_b_max - s_range_b_min) * 0.03f);
            float cur_span_b = s_range_b_max - s_range_b_min;
            bool want_shrink_b = (cur_span_b > 1e-9f && (b_max - b_min) < cur_span_b * 0.4f);
            if (outside_b) {
                s_range_b_min = b_min;
                s_range_b_max = b_max;
                s_shrink_cnt_b = 0;
            } else if (want_shrink_b) {
                s_shrink_cnt_b++;
                if (s_shrink_cnt_b >= RANGE_SHRINK_FRAMES) {
                    s_range_b_min = b_min;
                    s_range_b_max = b_max;
                    s_shrink_cnt_b = 0;
                }
            } else {
                s_shrink_cnt_b = 0;
            }
        }
    }

    /* 轴量程与刻度统一加 5% 上下留白：
     * ⚠ 之前 chart range 用分位原值 [min,max]，但刻度显示 [min-pad,max+pad]，
     * 导致曲线峰谷顶到 chart 框边、而刻度却说上方还有空间——视觉不一致。
     * 修复：chart range 与刻度都用留白后的范围，峰谷落在 95% 处不顶边。 */
    const float pad_a = (s_range_a_max - s_range_a_min) * 0.05f;
    const float pad_b = (s_range_b_max - s_range_b_min) * 0.05f;
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y,
                            (int32_t)((s_range_a_min - pad_a) * CHART_VALUE_SCALE),
                            (int32_t)((s_range_a_max + pad_a) * CHART_VALUE_SCALE));
    if (d->has_b) {
        lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_SECONDARY_Y,
                                (int32_t)((s_range_b_min - pad_b) * CHART_VALUE_SCALE),
                                (int32_t)((s_range_b_max + pad_b) * CHART_VALUE_SCALE));
    }

    /* 轴刻度（上/中/下）：与 chart range 一致的留白范围 */
    char txt[16];
    const float avals[3] = { s_range_a_max + pad_a,
                             (s_range_a_min + s_range_a_max) * 0.5f,
                             s_range_a_min - pad_a };
    const float bvals[3] = { s_range_b_max + pad_b,
                             (s_range_b_min + s_range_b_max) * 0.5f,
                             s_range_b_min - pad_b };
    for (int k = 0; k < 3; k++) {
        format_axis(txt, sizeof(txt), d->ch_a, avals[k]);
        lv_label_set_text(s_axis_a[k], txt);
        if (d->has_b) {
            format_axis(txt, sizeof(txt), d->ch_b, bvals[k]);
            lv_label_set_text(s_axis_b[k], txt);
        }
    }
}

/**
 * @brief 趋势装饰更新：均值参考线 + 主轴 min/max 极值点（P0）
 *
 * 只反映**窗口内趋势总结**（极值与偏离程度），不做读值交互。
 * 坐标用 lv_chart_get_point_pos_by_id()（像素精度，自动含 padding 偏移）。
 *
 * 极值点防跳动：圆点钉住「窗口内同一个极值事件」，随窗口滚动平移；
 * 只有当该事件滑出窗口、或出现显著更极端的新值时才切换锚点。
 * 否则窗口内小幅波动会让 min/max 在多个相近点间反复横跳、圆点来回抖。
 */
static void trend_update(int n, const mode_desc_t *d)
{
    if (n <= 0 || s_chart == NULL) {
        return;
    }

    /* 主轴（曲线 a）窗口内 min/max 与均值：用**原值**（s_buf_a_raw），
     * 趋势总结不应被平滑抹掉峰谷。跳过无数据的 NaN 空槽。
     * 同一趟遍历顺便找「最接近已锚定极值的点」，供防跳动判定。 */
    float vmin = 0.0f, vmax = 0.0f, vsum = 0.0f;
    int imin = 0, imax = 0;
    int iclose_min = -1, iclose_max = -1;
    float dclose_min = 1e30f, dclose_max = 1e30f;
    bool has_any = false;
    int cnt = 0;
    const float ext_thresh = (s_range_a_max - s_range_a_min) * 0.02f + 1e-6f;
    for (int i = 0; i < n; i++) {
        const float v = s_buf_a_raw[i];
        if (!isfinite(v)) {
            continue;
        }
        if (!has_any) {
            vmin = vmax = v;
            imin = imax = i;
            has_any = true;
        }
        vsum += v;
        cnt++;
        if (v < vmin) {
            vmin = v;
            imin = i;
        }
        if (v > vmax) {
            vmax = v;
            imax = i;
        }
        if (isfinite(s_hold_min_val)) {
            float dd = fabsf(v - s_hold_min_val);
            if (dd < dclose_min) {
                dclose_min = dd;
                iclose_min = i;
            }
        }
        if (isfinite(s_hold_max_val)) {
            float dd = fabsf(v - s_hold_max_val);
            if (dd < dclose_max) {
                dclose_max = dd;
                iclose_max = i;
            }
        }
    }
    if (!has_any) {
        /* 窗口内全空：隐藏趋势装饰，避免残留上一帧的位置 */
        if (s_avg_line != NULL) {
            lv_obj_set_hidden(s_avg_line, true);
        }
        if (s_dot_min != NULL) {
            lv_obj_set_hidden(s_dot_min, true);
        }
        if (s_dot_max != NULL) {
            lv_obj_set_hidden(s_dot_max, true);
        }
        return;
    }
    float vavg = vsum / (float)cnt;

    /* 防跳动锚点选择：
     *   ① 已锚定且锚点事件仍在窗口 → 继续钉住它（随滚动平移，不横跳）；
     *   ② 出现显著更极端的新值（超出阈值）→ 换锚到新极值；
     *   ③ 锚点事件已滑出窗口 → 回退到窗口内当前 min/max。 */
    int i_min_dot, i_max_dot;
    if (!isfinite(s_hold_min_val)) {
        s_hold_min_val = vmin;
        i_min_dot = imin;
    } else if (iclose_min >= 0 && dclose_min < ext_thresh) {
        i_min_dot = iclose_min;
    } else {
        s_hold_min_val = vmin;
        i_min_dot = imin;
    }
    if (!isfinite(s_hold_max_val)) {
        s_hold_max_val = vmax;
        i_max_dot = imax;
    } else if (iclose_max >= 0 && dclose_max < ext_thresh) {
        i_max_dot = iclose_max;
    } else {
        s_hold_max_val = vmax;
        i_max_dot = imax;
    }

    /* 均值参考线（水平 1px 浅色，跨图面宽）：按主轴量程（含 5% 留白）换算 y。
     * ⚠ 必须与 update_axis_range 中 chart_set_axis_range 的留白范围一致，
     * 否则均值线位置与曲线实际位置不匹配。 */
    int32_t y_avg;
    {
        const float pad = (s_range_a_max - s_range_a_min) * 0.05f;
        float lo = s_range_a_min - pad, hi = s_range_a_max + pad;
        float t = (hi > lo) ? (vavg - lo) / (hi - lo) : 0.5f;
        if (t < 0.0f) {
            t = 0.0f;
        } else if (t > 1.0f) {
            t = 1.0f;
        }
        lv_coord_t ch = (lv_coord_t)lv_obj_get_content_height(s_chart);
        y_avg = (lv_coord_t)((1.0f - t) * (float)(ch - 1));
    }
    if (s_avg_line != NULL) {
        lv_obj_set_hidden(s_avg_line, false);
        lv_obj_set_width(s_avg_line, lv_obj_get_content_width(s_chart));
        lv_obj_set_pos(s_avg_line, 0, y_avg);
    }

    /* min/max 极值点：深色小圆点钉在对应数据点。
     * 不用曲线同色（绿点压绿线几乎不可见）；深色在任何通道色上都清晰。 */
    if (s_dot_min != NULL && s_ser_a != NULL) {
        lv_point_t pm;
        lv_chart_get_point_pos_by_id(s_chart, s_ser_a, (uint32_t)i_min_dot, &pm);
        lv_obj_set_pos(s_dot_min, pm.x - 2, pm.y - 2);
        lv_obj_set_hidden(s_dot_min, false);
    }
    if (s_dot_max != NULL && s_ser_a != NULL) {
        lv_point_t px;
        lv_chart_get_point_pos_by_id(s_chart, s_ser_a, (uint32_t)i_max_dot, &px);
        lv_obj_set_pos(s_dot_max, px.x - 2, px.y - 2);
        lv_obj_set_hidden(s_dot_max, false);
    }
    (void)d;
}

void screen_wave_refresh(void)
{
    if (s_chart == NULL) {
        return;
    }

    const mode_desc_t *d = &s_mode_desc[s_mode];

    /* 时间槽契约（2026-10 重设计）：pm_wave_copy 填满 CHART_MAX_POINTS 个
     * 「固定时长」槽，索引 j 即屏幕第 j 列（右端 = 现在），无数据槽为 NaN。
     * 每槽时长恒为 窗长/点数，故波形走动速度与缓冲区填充程度、实际采样率
     * 都无关——修复"未占满时慢、占满后飞快"的观感问题。UI 一比一映射即可，
     * 不再需要右对齐与填充点数计算。 */
    (void)pm_wave_copy(s_window, s_mode, s_buf_a_raw, s_buf_b, CHART_MAX_POINTS);

    /* 平滑（可选）：对主轴/副轴相邻三个**都有效**的槽做 3 点移动平均；
     * 空槽（NaN）与端点保持原值，避免把断线抹成假波形。
     * 【Step2 降噪】副轴（常为电流，空载噪声大）同样平滑——原先只平滑主轴。 */
    settings_t scfg;
    bool smooth = (settings_get(&scfg) == ESP_OK && scfg.wave_smooth);
    for (int i = 0; i < CHART_MAX_POINTS; i++) {
        s_buf_a[i] = s_buf_a_raw[i];
        s_buf_b_raw[i] = s_buf_b[i];
    }
    if (smooth) {
        for (int i = 1; i < CHART_MAX_POINTS - 1; i++) {
            if (isfinite(s_buf_a_raw[i - 1]) && isfinite(s_buf_a_raw[i]) &&
                isfinite(s_buf_a_raw[i + 1])) {
                s_buf_a[i] = (s_buf_a_raw[i - 1] + s_buf_a_raw[i] + s_buf_a_raw[i + 1]) / 3.0f;
            }
            if (d->has_b && isfinite(s_buf_b_raw[i - 1]) && isfinite(s_buf_b_raw[i]) &&
                isfinite(s_buf_b_raw[i + 1])) {
                s_buf_b[i] = (s_buf_b_raw[i - 1] + s_buf_b_raw[i] + s_buf_b_raw[i + 1]) / 3.0f;
            }
        }
    }

    update_axis_range(CHART_MAX_POINTS);

    /* 一比一映射到 chart 槽位：j → 第 j 列；NaN → LV_CHART_POINT_NONE（断线） */
    for (int k = 0; k < CHART_MAX_POINTS; k++) {
        if (isfinite(s_buf_a[k])) {
            lv_chart_set_series_value_by_id(s_chart, s_ser_a, (uint32_t)k,
                                           (int32_t)(s_buf_a[k] * CHART_VALUE_SCALE));
        } else {
            lv_chart_set_series_value_by_id(s_chart, s_ser_a, (uint32_t)k, LV_CHART_POINT_NONE);
        }
        if (d->has_b && isfinite(s_buf_b[k])) {
            lv_chart_set_series_value_by_id(s_chart, s_ser_b, (uint32_t)k,
                                           (int32_t)(s_buf_b[k] * CHART_VALUE_SCALE));
        } else {
            lv_chart_set_series_value_by_id(s_chart, s_ser_b, (uint32_t)k, LV_CHART_POINT_NONE);
        }
    }
    lv_chart_refresh(s_chart);
#if WAVE_DECOR_ENABLE
    trend_update(CHART_MAX_POINTS, d); /* 均值线 + min/max 极值点 */
#else
    (void)d;
#endif

    /* 底部实时/平均数据行：跳过无数据的 NaN 空槽 */
    float a_last = 0.0f, b_last = 0.0f, a_avg = 0.0f, b_avg = 0.0f;
    int cnt = 0;
    double sa = 0.0, sb = 0.0;
    for (int k = 0; k < CHART_MAX_POINTS; k++) {
        if (isfinite(s_buf_a[k])) {
            sa += s_buf_a[k];
            cnt++;
            a_last = s_buf_a[k]; /* 最后一个有效值即"实时" */
        }
        if (d->has_b && isfinite(s_buf_b[k])) {
            sb += s_buf_b[k];
            b_last = s_buf_b[k];
        }
    }
    if (cnt == 0) {
        return;
    }
    a_avg = (float)(sa / cnt);
    b_avg = d->has_b ? (float)(sb / cnt) : 0.0f;

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
 * @brief chart 尺寸确定后摆放 6 个轴刻度（浮于曲线之上，左右贴边）
 *
 * 2026-10 改：波形铺满整宽，刻度不再占 padding 留白（原先两侧各让出
 * 66px，曲线被挤在中间很不协调）。刻度改贴左右边缘、加半透明白底，
 * 压在曲线之上仍可读。
 */
static void chart_size_event_cb(lv_event_t *e)
{
    lv_obj_t *chart = lv_event_get_current_target(e);
    lv_coord_t w = lv_obj_get_content_width(chart);
    lv_coord_t h = lv_obj_get_content_height(chart);
    for (int k = 0; k < 3; k++) {
        lv_coord_t y = (k == 0) ? -3 : (k == 1) ? (h - 14) / 2 : (h - 11);
        lv_obj_set_pos(s_axis_a[k], 0, y);            /* 左刻度贴左缘 */
        lv_obj_set_pos(s_axis_b[k], w - AXIS_LABEL_W, y); /* 右刻度贴右缘 */
    }
}

/** 曲线平滑开关：写 settings 并持久化（立即生效，下一次刷新即用新设置） */
static void smooth_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    (void)settings_set_wave_smooth(on ? 1u : 0u);
    (void)settings_save_now();
}

static void create_panel(lv_obj_t *scr, const wave_geom_t *g)
{
    s_panel = lv_obj_create(scr);
    lv_obj_set_size(s_panel, LV_PCT(100), PANEL_H);
    lv_obj_set_pos(s_panel, 0, g->h); /* 初始沉在屏外（横 240 / 竖 284） */
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
    lv_obj_set_size(col_w, g->col_win_w, 88);
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
    lv_obj_set_size(s_roller_win, g->col_win_w - 8, 68);
    /* 两阶段触控：初始禁滑动（防下滑关闭菜单时误改），点击选中后才可滑 */
    lv_obj_set_scrollable(s_roller_win, false);
    lv_obj_add_event_cb(s_roller_win, roller_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *col_m = lv_obj_create(row);
    lv_obj_remove_style_all(col_m);
    lv_obj_set_size(col_m, g->col_mode_w, 88);
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
    lv_obj_set_size(s_roller_mode, g->col_mode_w - 8, 68);
    /* 两阶段触控：同时间窗 roller */
    lv_obj_set_scrollable(s_roller_mode, false);
    lv_obj_add_event_cb(s_roller_mode, roller_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* 曲线平滑开关（settings.wave_smooth，3 点移动平均） */
    lv_obj_t *smooth_row = lv_obj_create(s_panel);
    lv_obj_remove_style_all(smooth_row);
    lv_obj_set_size(smooth_row, LV_PCT(100), 28);
    lv_obj_set_scrollable(smooth_row, false);
    lv_obj_set_flex_flow(smooth_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(smooth_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *smooth_lbl = lv_label_create(smooth_row);
    lv_label_set_text(smooth_lbl, "曲线平滑");
    lv_obj_set_style_text_font(smooth_lbl, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(smooth_lbl, COL_DARK, 0);
    lv_obj_t *sw = lv_switch_create(smooth_row);
    /* 与设置页自动旋转开关统一样式：关闭轨道灰、开启轨道品牌蓝、
     * 白色旋钮带 1px 灰边框（避免白底上旋钮"消失"）。 */
    lv_obj_set_size(sw, 46, 24);
    lv_obj_set_style_bg_color(sw, COL_CARD_BD, 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sw, COL_PRIMARY, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    /* 旋钮边框用深灰：与设置页自动旋转开关一致，白底上清晰可辨 */
    lv_obj_set_style_border_color(sw, COL_GRAY, LV_PART_KNOB);
    lv_obj_set_style_border_width(sw, 1, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 3, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    settings_t scfg;
    if (settings_get(&scfg) == ESP_OK && scfg.wave_smooth) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, smooth_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 提示分两行：单行 26 字符在 252px 内宽里放不下（12px 字约 280px），
     * 会被左右裁切，竖屏 240 宽更严重 */
    lv_obj_t *hint1 = lv_label_create(s_panel);
    lv_label_set_text(hint1, "点选项目后可滑动 · K1/K2 上下");
    lv_obj_set_style_text_font(hint1, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(hint1, COL_GRAY, 0);

    lv_obj_t *hint2 = lv_label_create(s_panel);
    lv_label_set_text(hint2, "K2 长按确定 · K1 长按退出");
    lv_obj_set_style_text_font(hint2, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(hint2, COL_GRAY, 0);
}

void screen_wave_create(lv_obj_t *tile)
{
    wave_geom_t g = wave_geom();

    /* ---- 旋转重建清理：底部滑板挂在 screen 层（不随 tile 删除），
     * 必须先停掉位移动画再删对象，否则动画回调会访问已释放的 panel ---- */
    if (s_panel != NULL) {
        lv_anim_del(s_panel, anim_panel_y);
        lv_obj_delete(s_panel);
        s_panel = NULL;
    }
    s_menu_open = false;
    s_panel_animating = false;
    s_chart = NULL;
    s_ser_a = NULL;
    s_ser_b = NULL;
    s_lbl_title = NULL;
    s_lbl_time_l = NULL;
    s_lbl_time_r = NULL;
    s_lbl_realtime = NULL;
    s_lbl_average = NULL;
    s_roller_win = NULL;
    s_roller_mode = NULL;
    for (int k = 0; k < 3; k++) {
        s_axis_a[k] = NULL;
        s_axis_b[k] = NULL;
    }
    /* s_window / s_mode / 量程滞回状态保留：用户的波形选择不被旋转打断 */

    lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_style_pad_all(tile, 0, 0);

    /* 标题（居中，天然避开圆角） */
    s_lbl_title = lv_label_create(tile);
    lv_label_set_text(s_lbl_title, "");
    lv_obj_set_style_text_font(s_lbl_title, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(s_lbl_title, COL_DARK, 0);
    lv_obj_set_pos(s_lbl_title, 0, 26);
    lv_obj_set_width(s_lbl_title, g.w);
    lv_obj_set_style_text_align(s_lbl_title, LV_TEXT_ALIGN_CENTER, 0);

    /* 图表 */
    s_chart = lv_chart_create(tile);
    lv_obj_set_size(s_chart, g.w - 2 * SAFE_X, g.chart_h);
    lv_obj_set_pos(s_chart, SAFE_X, g.chart_y);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_MAX_POINTS);
    /* 每帧用 set_series_value_by_id 整帧重写全部槽位（时间槽映射，不是
     * "逐点追加"），update_mode 只影响 set_next_value，这里无需设置。 */
    /* 格线密度：LVGL 的 div_line_count 是「线数」，格数 = 线数 - 1。
     * 波形重构 Step1（做减法）：5×10 格 → 4×8 格（线数 5×9），
     * 暗灰 1px，降低视觉密度——DSO Nano 等小屏示波器的通用密度。 */
    lv_chart_set_div_line_count(s_chart, 5, 9);
    lv_obj_set_style_pad_left(s_chart, 4, 0);  /* 曲线铺满宽度，Y 轴刻度浮于其上 */
    lv_obj_set_style_pad_right(s_chart, 4, 0); /* （原 AXIS_LABEL_W+4 让出两侧，波形被挤压） */
    lv_obj_set_style_pad_top(s_chart, 5, 0);
    lv_obj_set_style_pad_bottom(s_chart, 5, 0);
    /* 黑底 + 暗灰格线（示波器风格，见 COL_WAVE_BG 注释） */
    lv_obj_set_style_bg_color(s_chart, COL_WAVE_BG, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_chart, COL_WAVE_GRID, 0);
    lv_obj_set_style_border_width(s_chart, 1, 0);
    lv_obj_set_style_radius(s_chart, 8, 0);
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_chart, COL_WAVE_GRID, LV_PART_MAIN); /* 格线暗灰 */
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);
    /* 波形图自适应屏宽、禁用自身滚动：左右轴刻度标签挂在 chart 的
     * padding 留白区（负 x / 越界 x），此前 chart 可滚动时把它们算进
     * 滚动范围，出现左右滑动空间与滚动条；置 scrollable=false 后
     * 刻度仍显示在留白区内（未越出 border box），但不再产生滚动，
     * 横滑交还给 tileview 翻页。 */
    lv_obj_set_scrollable(s_chart, false);
    lv_obj_set_scrollbar_mode(s_chart, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_chart, chart_size_event_cb, LV_EVENT_SIZE_CHANGED, NULL);

    s_ser_a = lv_chart_add_series(s_chart, COL_V, LV_CHART_AXIS_PRIMARY_Y);
    s_ser_b = lv_chart_add_series(s_chart, COL_I, LV_CHART_AXIS_SECONDARY_Y);
    lv_chart_set_all_values(s_chart, s_ser_a, LV_CHART_POINT_NONE);
    lv_chart_set_all_values(s_chart, s_ser_b, LV_CHART_POINT_NONE);

    /* 轴刻度（上/中/下 × 左/右），浮于曲线之上：深色半透底保证黑底上可读 */
    const lv_font_t *f12 = lvgl_port_get_font(LVGL_PORT_FONT_12);
    for (int k = 0; k < 3; k++) {
        s_axis_a[k] = lv_label_create(s_chart);
        lv_obj_set_size(s_axis_a[k], AXIS_LABEL_W, 14);
        lv_obj_set_style_text_font(s_axis_a[k], f12, 0);
        lv_obj_set_style_pad_hor(s_axis_a[k], 2, 0);
        lv_label_set_text(s_axis_a[k], "--");
        lv_obj_set_style_bg_color(s_axis_a[k], COL_WAVE_BG, 0);
        lv_obj_set_style_bg_opa(s_axis_a[k], LV_OPA_70, 0);
        lv_obj_set_clickable(s_axis_a[k], false);
        lv_obj_set_scrollable(s_axis_a[k], false);

        s_axis_b[k] = lv_label_create(s_chart);
        lv_obj_set_size(s_axis_b[k], AXIS_LABEL_W, 14);
        lv_obj_set_style_text_font(s_axis_b[k], f12, 0);
        lv_obj_set_style_pad_hor(s_axis_b[k], 2, 0);
        lv_obj_set_style_text_align(s_axis_b[k], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(s_axis_b[k], "--");
        lv_obj_set_style_bg_color(s_axis_b[k], COL_WAVE_BG, 0);
        lv_obj_set_style_bg_opa(s_axis_b[k], LV_OPA_70, 0);
        lv_obj_set_clickable(s_axis_b[k], false);
        lv_obj_set_scrollable(s_axis_b[k], false);
    }

#if WAVE_DECOR_ENABLE
    /* 趋势装饰对象：均值参考线（1px 浅色横条）+ min/max 极值点（4px 圆），
     * 挂 chart 下、坐标相对 content area，样式独立不污染曲线 */
    s_avg_line = lv_obj_create(s_chart);
    lv_obj_remove_style_all(s_avg_line);
    lv_obj_set_size(s_avg_line, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(s_avg_line, COL_DARK, 0);
    lv_obj_set_style_bg_opa(s_avg_line, LV_OPA_50, 0);
    lv_obj_set_clickable(s_avg_line, false);
    lv_obj_set_scrollable(s_avg_line, false);
    lv_obj_move_to_index(s_avg_line, 0); /* 渲染在曲线之下 */

    s_dot_min = lv_obj_create(s_chart);
    lv_obj_remove_style_all(s_dot_min);
    lv_obj_set_size(s_dot_min, 5, 5);
    lv_obj_set_style_bg_color(s_dot_min, COL_DARK, 0);
    lv_obj_set_style_bg_opa(s_dot_min, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dot_min, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(s_dot_min, false);
    lv_obj_set_scrollable(s_dot_min, false);
    lv_obj_set_hidden(s_dot_min, true);

    s_dot_max = lv_obj_create(s_chart);
    lv_obj_remove_style_all(s_dot_max);
    lv_obj_set_size(s_dot_max, 5, 5);
    lv_obj_set_style_bg_color(s_dot_max, COL_DARK, 0);
    lv_obj_set_style_bg_opa(s_dot_max, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dot_max, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(s_dot_max, false);
    lv_obj_set_scrollable(s_dot_max, false);
    lv_obj_set_hidden(s_dot_max, true);
#endif

    /* 时间轴（左：-窗长，中：-半窗，右：0）贴 chart 图面左右缘 */
    s_lbl_time_l = lv_label_create(tile);
    lv_label_set_text(s_lbl_time_l, "");
    lv_obj_set_style_text_font(s_lbl_time_l, f12, 0);
    lv_obj_set_style_text_color(s_lbl_time_l, COL_GRAY, 0);
    lv_obj_set_pos(s_lbl_time_l, SAFE_X, g.time_y);

    s_lbl_time_m = lv_label_create(tile);
    lv_label_set_text(s_lbl_time_m, "");
    lv_obj_set_style_text_font(s_lbl_time_m, f12, 0);
    lv_obj_set_style_text_color(s_lbl_time_m, COL_GRAY, 0);
    lv_obj_set_width(s_lbl_time_m, 60);
    lv_obj_set_style_text_align(s_lbl_time_m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_time_m, SAFE_X + (g.w - 2 * SAFE_X - 60) / 2, g.time_y);

    s_lbl_time_r = lv_label_create(tile);
    lv_label_set_text(s_lbl_time_r, "0");
    lv_obj_set_style_text_font(s_lbl_time_r, f12, 0);
    lv_obj_set_style_text_color(s_lbl_time_r, COL_GRAY, 0);
    lv_obj_set_width(s_lbl_time_r, 30);
    lv_obj_set_style_text_align(s_lbl_time_r, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_lbl_time_r, g.w - SAFE_X - 30, g.time_y);

    /* 实时 / 平均 */
    s_lbl_realtime = lv_label_create(tile);
    lv_label_set_text(s_lbl_realtime, "实 --");
    lv_obj_set_style_text_font(s_lbl_realtime, f12, 0);
    lv_obj_set_style_text_color(s_lbl_realtime, COL_DARK, 0);
    lv_obj_set_pos(s_lbl_realtime, SAFE_X, g.rt_y);

    s_lbl_average = lv_label_create(tile);
    lv_label_set_text(s_lbl_average, "均 --");
    lv_obj_set_style_text_font(s_lbl_average, f12, 0);
    lv_obj_set_style_text_color(s_lbl_average, COL_GRAY, 0);
    lv_obj_set_pos(s_lbl_average, SAFE_X, g.av_y);

    update_header();
    create_panel(lv_screen_active(), &g);
}

pm_wave_window_t screen_wave_get_window(void)
{
    return s_window;
}

pm_wave_mode_t screen_wave_get_mode(void)
{
    return s_mode;
}
