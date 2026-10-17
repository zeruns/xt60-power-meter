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
 *
 * 进阶机制（2026-10 波形重构，详见各函数注释）：
 *   · **时间槽桶聚合**：pm_wave_copy 把整窗切成 CHART_MAX_POINTS 个固定时长
 *     槽，索引即屏幕列、右端=现在。槽宽与采样周期不同步时旧"最近样本+容差"
 *     方案会约半数槽落空断线，改桶聚合后每槽恒定覆盖 窗长/点数 的时间；
 *   · **min/max 包络防混叠**：480 槽 > 屏宽 280 像素，多槽挤进同一像素列，
 *     LVGL 对 LINE 系列自动画 min/max 竖线包络（示波器 peak-detect 同款），
 *     毛刺尖峰不会因抽稀被丢掉；
 *   · **1-2-5 标准格值自动量程**（wave_nice_div）：每格取 1/2/5×10ⁿ 标准格值
 *     （≥1V/格、≥0.1A/格），下边界锚定格值整数倍，5 条格线全部落在整格值
 *     上，读数可按格数心算；上下留 12% 呼吸空间；
 *   · **量程滞回 + 收缩防抖**：扩程立即（带 3% 余量防噪声触发）、收缩需连续
 *     N 帧确认，单帧误判不会让量程在两个值间横跳；
 *   · **定宽列数据行**（layout_data_cols）：每个测量量占一列，数值右对齐、
 *     单位固定列右端，数字变宽向左生长，彻底消除"数值左右晃"；
 *   · **roller 两阶段触控**（s_roller_edit）：点击选中后才允许滑动，防下滑
 *     关菜单时误改选项；
 *   · **三页循环滑动**：测量/波形/设置三页由 ui_nav 的 tileview 承载，
 *     indev 级 LV_EVENT_RELEASED 按「按下记起点、抬起算总位移」判定横/竖滑
 *     （横滑翻页、竖滑开/关本页设置滑板），本文件提供 screen_wave_swipe_cb。
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
#define COL_DARK lv_color_hex(0x1A1A1A) /* 近黑文字（白底主文字/装饰点） */
#define COL_GRAY lv_color_hex(0x6B7280) /* 中灰文字（次级信息：单位/时间轴） */
#define COL_CARD_BD lv_color_hex(0xB4BEC9) /* 边框：与白底拉开可辨差异 */
#define COL_PANEL_BG lv_color_hex(0xF1F4F8) /* 设置滑板浅灰蓝底 */
#define COL_PRIMARY lv_color_hex(0x0066CC) /* 品牌蓝（与设置页一致，用于开关开启态） */

#define CHART_MAX_POINTS 480 /**< 图表槽位数＝时间槽数（每槽一列，见文件头） */
#define CHART_VALUE_SCALE 100.0f /**< 值放大倍数：LVGL 图表只收整数，×100 保留 2 位小数 */
/** 量程上下留白比例：12%（信号占图面约 3/4 高度，波峰波谷不贴边） */
#define WAVE_RANGE_PAD_FRAC 0.12f
/** Y 轴格数（hdiv 线数 - 1，与 chart 创建的 lv_chart_set_div_line_count 一致） */
#define WAVE_Y_DIVS 4
/** 电压波形每格最小分辨率（V）：防止微小信号被过放大（用户要求 ≥1V/格） */
#define WAVE_MIN_DIV_V 1.0f
/** 电流波形每格最小分辨率（A）：防止微小信号被过放大（用户要求 ≥0.1A/格） */
#define WAVE_MIN_DIV_A 0.1f

/**
 * @brief 1-2-5 标准格值序列：取 ≥ min_div 的最小标准格值
 *
 * 仪表行业惯例格值序列（万用表/示波器通用）：
 *   电压基于 1V：1 / 2 / 5 / 10 / 20 / 50 V…；
 *   电流基于 0.1A：0.1 / 0.2 / 0.5 / 1 / 2 / 5 A…。
 * 格值取标准值、刻度锚定格值整数倍，读数时可直接按格数心算
 * （用户要求"格子都是整数，如 1V、2V、0.1A、0.5A、1A、2A"）。
 *
 * @param min_div 期望的最小格值（每格至少要能分辨这么多物理量）
 * @param is_volt true=电压（序列基值 1V）/ false=电流（基值 0.1A）
 * @return ≥ min_div 的最小 1-2-5 标准格值
 */
static float wave_nice_div(float min_div, bool is_volt)
{
    const float base = is_volt ? WAVE_MIN_DIV_V : WAVE_MIN_DIV_A; /* 序列基值：1V / 0.1A */
    static const float mant[3] = { 1.0f, 2.0f, 5.0f }; /* 有效数字：1/2/5 */
    float scale = 1.0f;
    /* 双层循环枚举 1-2-5×10ⁿ 序列：外层升数量级、内层枚举有效数字，
     * 取第一个 ≥ min_div 的候选即为答案（序列本身递增）。 */
    for (int dec = 0; dec < 5; dec++) {
        for (int m = 0; m < 3; m++) {
            const float cand = base * mant[m] * scale;
            if (cand >= min_div) {
                return cand;
            }
        }
        scale *= 10.0f;
    }
    return base * 100000.0f; /* 兜底（不会到达） */
}
/* 滑板高度：title 20 + row 88 + 平滑行 28+4 + 提示两行 32 + 行距 4×4 +
 * 内边距 20 ≈ 224。旧值 188 是加"曲线平滑"开关行之前的布局。 */
#define PANEL_H 224
#define SAFE_X 12          /**< 内容区左右安全边距（避屏幕圆角） */
#define AXIS_LABEL_W 62    /**< 纵轴刻度标签宽（容得下 "19.98 V" 之类） */

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

/**
 * @brief 取当前屏幕方向对应的布局几何
 *
 * @return 几何参数（横/竖屏两套硬编码值；display 缺失时按横屏 284×240 兜底）
 * @note 用 lv_display_get_horizontal/vertical_resolution 读**逻辑分辨率**
 *       （LVGL 9 的显示对象默认 display），旋转后分辨率交换，故以宽高
 *       大小关系判断横竖屏，而非记忆上次方向。
 */
static wave_geom_t wave_geom(void)
{
    lv_display_t *d = lv_display_get_default(); /* 取默认显示对象（LVGL 9 把 disp 改名 display） */
    int32_t w = (d != NULL) ? lv_display_get_horizontal_resolution(d) : 284;
    int32_t h = (d != NULL) ? lv_display_get_vertical_resolution(d) : 240;
    wave_geom_t g;
    g.w = w;
    g.h = h;
    if (w >= h) {
        /* 横屏 284×240：数值 16px 行高约 20px，两行错开 22px（间隔大于行高，
         * 视觉分组清晰）；均行底 ≤ 236 离底边留 4px，避开圆形屏圆角裁切 */
        g.chart_y = 44;
        g.chart_h = 132;
        g.time_y = 178;
        g.rt_y = 194;
        g.av_y = 216;
        g.col_win_w = 116;
        g.col_mode_w = 146;
    } else {
        /* 竖屏 240×284：图表加高，时间轴/数据行整体下移 */
        g.chart_y = 44;
        g.chart_h = 166;
        g.time_y = 217;
        g.rt_y = 233;
        g.av_y = 255;
        g.col_win_w = 96;
        g.col_mode_w = 104;
    }
    return g;
}

/* ---- 波形页 LVGL 对象与页面状态（文件级 static，页面重建时重置/保留策略见
 * screen_wave_create 注释） ---- */
static lv_obj_t *s_chart = NULL;              /**< 图表控件（lv_chart） */
static lv_chart_series_t *s_ser_a = NULL;     /**< 主轴曲线（左刻度，CH1 位） */
static lv_chart_series_t *s_ser_b = NULL;     /**< 副轴曲线（右刻度，CH2 位） */
static lv_obj_t *s_lbl_title;                 /**< 顶部标题："模式 · 窗长" */
static lv_obj_t *s_lbl_time_l;                /**< 时间轴左刻度 "-窗长" */
static lv_obj_t *s_lbl_time_r;                /**< 时间轴右刻度 "0"（现在） */
static lv_obj_t *s_axis_a[3];                 /**< 主轴刻度（上/中/下 ×3） */
static lv_obj_t *s_axis_b[3];                 /**< 副轴刻度（上/中/下 ×3） */

static pm_wave_window_t s_window = PM_WAVE_10S; /**< 当前时间窗（B20：create 时从 settings 恢复） */
static pm_wave_mode_t s_mode = PM_WAVE_MODE_VI; /**< 当前波形模式（B20：create 时从 settings 恢复） */

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

static float s_buf_a[CHART_MAX_POINTS];      /**< 主轴绘图值（平滑后，喂给 chart） */
static float s_buf_b[CHART_MAX_POINTS];      /**< 副轴绘图值（平滑后） */
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

/* ---- 底部「实时/平均」数据行：定宽列布局 ----
 * 【问题】原先整行拼成一个字符串（"实 19.97670V 0.01920A 0.38348W"），
 * 数值正负号跳动/位数变化会让后面所有数字左右晃，观测时很难盯住一个量。
 * 【方案】每个测量量占一个**定宽列**，列内数值右对齐（定宽框）+ 单位固定在
 * 列右端——数值变宽时向左生长、单位与列边界纹丝不动，彻底消除位置跳动。
 * 列宽按屏宽均分，自适应横/竖屏；列数随波形模式（1~3 列）变化。 */
#define WAVE_DATA_COLS 3      /**< 最多列数：电压+电流+功率 */
#define WAVE_DATA_PREFIX_W 16 /**< 行首"实"/"均"字样占宽（14px 字体） */
#define WAVE_DATA_UNIT_W   14 /**< 单位列定宽（12px 字体，容得下 "Wh"） */
#define WAVE_DATA_VAL_GAP  2  /**< 数值框与单位之间固定间隔 */
static lv_obj_t *s_rt_prefix = NULL;
static lv_obj_t *s_av_prefix = NULL;
static lv_obj_t *s_rt_val[WAVE_DATA_COLS] = { NULL };
static lv_obj_t *s_av_val[WAVE_DATA_COLS] = { NULL };
static lv_obj_t *s_rt_unit[WAVE_DATA_COLS] = { NULL };
static lv_obj_t *s_av_unit[WAVE_DATA_COLS] = { NULL };
static int s_data_ncol = 0; /**< 当前列数（变化时才重新排版，避免每帧 set_pos） */

/** 波形模式描述：每种模式画哪两个通道、单位是什么（索引 = pm_wave_mode_t） */
typedef struct {
    int8_t ch_a;        /**< 主轴通道号：0=V 1=I 2=P 3=E */
    int8_t ch_b;        /**< 副轴通道号（无副轴时 -1） */
    bool has_b;         /**< 是否有第二条曲线 */
    const char *unit_a; /**< 主轴单位字符串 */
    const char *unit_b; /**< 副轴单位字符串 */
} mode_desc_t;

/** 5 种波形模式的通道/单位映射表（用 [枚举名] = 显式下标，与枚举顺序解耦） */
static const mode_desc_t s_mode_desc[PM_WAVE_MODE_MAX] = {
    [PM_WAVE_MODE_P]  = { 2, -1, false, "W",  "" },
    [PM_WAVE_MODE_IP] = { 1, 2, true,  "A",  "W" },
    [PM_WAVE_MODE_VP] = { 0, 2, true,  "V",  "W" },
    [PM_WAVE_MODE_VI] = { 0, 1, true,  "V",  "A" },
    [PM_WAVE_MODE_E]  = { 3, -1, false, "Wh", "" },
};

/**
 * @brief 通道号 → 曲线颜色（V 绿 / I 蓝 / P 橙 / E 紫，见顶部 COL_* 宏）
 *
 * @param ch 通道号 0~3（0=V 1=I 2=P 其余=E）
 * @return 该通道的曲线颜色
 */
static lv_color_t channel_color(int ch)
{
    switch (ch) {
    case 0: return COL_V;
    case 1: return COL_I;
    case 2: return COL_P;
    default: return COL_E;
    }
}

/**
 * @brief 按通道单位格式化轴值（数值 + 空格 + 单位后缀），用于纵轴刻度
 *
 * 小数位按通道固定（V 2 位 / A 3 位 / W 1 位 / Wh 3 位），
 * 刻度文本宽度稳定、不随数值跳动。
 *
 * @param dst  输出缓冲
 * @param n    缓冲大小
 * @param ch   通道号 0~3（0=V 1=I 2=P 3=Wh）
 * @param v    待格式化数值
 */
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

/**
 * @brief 只格式化数值（不带单位）——底部数据列的单位固定在列右端
 *
 * 小数位按通道单位固定（V 2 位 / A 3 位 / W 1 位 / Wh 3 位），
 * 保证同一列内数字宽度恒定，负号跳动时只有符号位变化、末位不跳。
 *
 * @param dst  输出缓冲
 * @param n    缓冲大小
 * @param ch   通道号 0~3（决定小数位；-1 等无效值走 default 3 位）
 * @param v    待格式化数值
 */
static void format_axis_val(char *dst, size_t n, int ch, float v)
{
    const char *fmt;
    switch (ch) {
    case 0: fmt = "%.2f"; break;
    case 1: fmt = "%.3f"; break;
    case 2: fmt = "%.1f"; break;
    default: fmt = "%.3f"; break;
    }
    snprintf(dst, n, fmt, (double)v);
}

/**
 * @brief 摆放底部「实时/平均」数据列（列数变化时才调用）
 *
 * 布局：[前缀 24px][列 0: 数值(定宽) + 单位(定宽)] × ncol
 * 列宽 = (屏宽 - 2×SAFE_X - 前缀宽) / ncol；每列内数值右对齐、单位固定右端。
 * 数值框宽 = 列宽 - 单位宽 - 固定间隔，负号/位数变化时数字向左生长不推挤。
 *
 * @param g 布局几何（列宽随屏宽均分，自适应横/竖屏）
 */
static void layout_data_cols(const wave_geom_t *g)
{
    const int ncol = s_data_ncol;
    if (ncol <= 0) {
        return;
    }
    /* 列可用宽（自适应屏宽），每列均分 */
    const int32_t avail = g->w - 2 * SAFE_X - WAVE_DATA_PREFIX_W;
    const int32_t col_w = avail / ncol;
    const int32_t val_w = col_w - WAVE_DATA_UNIT_W - WAVE_DATA_VAL_GAP;
    /* 行高按数值字体（16px）计算；单位 12px 垂直居中于行内——
     * 若单位与数值同 y 顶对齐，会飘在右上角像"上标"（实机照片反馈） */
    const lv_font_t *f_val = lvgl_port_get_font(LVGL_PORT_FONT_16);
    const lv_font_t *f_unit = lvgl_port_get_font(LVGL_PORT_FONT_12);
    const lv_coord_t line_h = (lv_coord_t)(f_val->line_height + 2);
    const lv_coord_t unit_dy = (lv_coord_t)((line_h - f_unit->line_height) / 2);

    lv_obj_t *prefix[2] = { s_rt_prefix, s_av_prefix };
    lv_obj_t **vals[2] = { s_rt_val, s_av_val };
    lv_obj_t **units[2] = { s_rt_unit, s_av_unit };
    const int32_t ys[2] = { g->rt_y, g->av_y };
    for (int row = 0; row < 2; row++) {
        lv_obj_set_pos(prefix[row], SAFE_X, ys[row] + 2); /* 设置对象相对父级的 x/y 位置 */
        for (int c = 0; c < WAVE_DATA_COLS; c++) {
            const bool on = (c < ncol);
            lv_obj_set_hidden(vals[row][c], !on); /* 隐藏多余列（不参与绘制） */
            lv_obj_set_hidden(units[row][c], !on);
            if (!on) {
                continue;
            }
            const int32_t x0 = SAFE_X + WAVE_DATA_PREFIX_W + c * col_w;
            /* 数值右对齐 + 定宽框：数字变宽向左生长，单位位置纹丝不动 */
            lv_obj_set_pos(vals[row][c], x0, ys[row]);
            lv_obj_set_size(vals[row][c], val_w, line_h); /* 定宽框（数值右对齐在其内） */
            /* 单位贴列右端、垂直居中 */
            lv_obj_set_pos(units[row][c], x0 + val_w + WAVE_DATA_VAL_GAP, ys[row] + unit_dy);
        }
    }
}

/**
 * @brief 更新两个 roller 的焦点边框（选中项加粗蓝色、未选中细灰）
 *
 * 无参数无返回值；读写全局 s_focus_idx / s_roller_win / s_roller_mode。
 */
static void update_focus_style(void)
{
    lv_obj_t *targets[2] = { s_roller_win, s_roller_mode };
    for (int i = 0; i < 2; i++) {
        /* lv_obj_set_style_* 的第 3 个参数 0 = 仅默认状态（不带 state 掩码的样式） */
        lv_obj_set_style_border_color(targets[i],
                                     (i == s_focus_idx) ? lv_color_hex(0x0066CC) : COL_CARD_BD,
                                     0);
        lv_obj_set_style_border_width(targets[i], (i == s_focus_idx) ? 2 : 1, 0);
    }
}

/**
 * @brief roller 点击回调（LV_EVENT_CLICKED）：两阶段触控的状态切换
 *
 * 触摸 roller 时被 LVGL 事件系统调用。同一 roller 再点一次＝确认退出编辑；
 * 点另一个 roller＝切换编辑对象。实现见文件级注释 s_roller_edit。
 *
 * @param e 事件对象（携带被点击的 roller）
 */
static void roller_clicked_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e); /* 取事件触发源对象 */
    int8_t idx = (obj == s_roller_win) ? 0 : 1;
    if (s_roller_edit == idx) {
        /* 已在编辑态：本次点击视为"确认当前选择"，退出编辑态 */
        s_roller_edit = -1;
        /* lv_obj_set_scrollable：开关对象自身的滑动能力（false 后手指拖不动） */
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

/**
 * @brief 关闭 roller 编辑态（关菜单/长按确认等调用）
 *
 * 无参数无返回值；两个 roller 的 scrollable 都恢复 false。
 */
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

/**
 * @brief 把两个 roller 的当前选中项落到页面状态（确认时调用）
 *
 * 无参数无返回值。只有真的发生变化才复位量程滞回/极值锚点——
 * 不变时保持现状，避免无意义的量程重新自适应。
 * @note lv_roller_get_selected 返回选项索引（0 起），此处先做越界检查再转换。
 */
static void apply_roller_selection(void)
{
    int w = lv_roller_get_selected(s_roller_win);
    int m = lv_roller_get_selected(s_roller_mode);
    bool changed = false;
    if (w >= 0 && w < PM_WAVE_WINDOW_MAX && (pm_wave_window_t)w != s_window) {
        s_window = (pm_wave_window_t)w;
        /* B20 波形持久化：时间窗写入 settings（内存缓存立即生效，防抖落盘） */
        (void)settings_set_wave_window((uint8_t)w);
        changed = true;
    }
    if (m >= 0 && m < PM_WAVE_MODE_MAX && (pm_wave_mode_t)m != s_mode) {
        s_mode = (pm_wave_mode_t)m;
        /* B20 波形持久化：波形模式写入 settings */
        (void)settings_set_wave_mode((uint8_t)m);
        changed = true;
    }
    if (changed) {
        s_range_valid = false; /* 换了窗/模式，量程重新自适应 */
        s_shrink_cnt_a = 0;
        s_shrink_cnt_b = 0;
        s_hold_min_val = NAN; /* 极值锚点失效，重新锚定 */
        s_hold_max_val = NAN;
        /* "确定"是离散动作（非拖动）：立即落盘，防马上断电丢失本次选择 */
        (void)settings_save_now();
    }
}

/**
 * @brief 刷新标题、时间轴与曲线配色（模式/窗长变化后调用）
 *
 * 标题格式 "模式名 · 窗长名"；时间轴三刻度（左 -窗长/中 -半窗/右 0）；
 * 再按当前模式给两条曲线与 6 个轴刻度换色、隐藏无副轴时刻度。
 */
static void update_header(void)
{
    char t[48];
    snprintf(t, sizeof(t), "%s · %s", pm_wave_mode_name(s_mode), pm_wave_window_name(s_window));
    lv_label_set_text(s_lbl_title, t); /* 设置 label 文本（自动重排版/重绘） */

    /* 时间轴：左 -窗长、中 -半窗、右 0（贴图面左右缘） */
    char tl[16];
    snprintf(tl, sizeof(tl), "-%s", pm_wave_window_name(s_window));
    lv_label_set_text(s_lbl_time_l, tl);
    lv_label_set_text(s_lbl_time_r, "0");
    if (s_lbl_time_m != NULL) {
        /* 半窗刻度：与 PM_WAVE_WINDOW_MAX 一一对应的"窗长一半"文本表 */
        static const char *const half_name[PM_WAVE_WINDOW_MAX] = {
            "0.5s", "5s", "15s", "30s", "60s", "5min",
        };
        char tm[16];
        snprintf(tm, sizeof(tm), "-%s", half_name[s_window % PM_WAVE_WINDOW_MAX]);
        lv_label_set_text(s_lbl_time_m, tm);
    }

    const mode_desc_t *d = &s_mode_desc[s_mode];
    lv_chart_set_series_color(s_chart, s_ser_a, channel_color(d->ch_a)); /* 改曲线系列颜色 */
    lv_chart_hide_series(s_chart, s_ser_b, !d->has_b); /* 隐藏/显示副轴系列（true=隐藏） */
    if (d->has_b) {
        lv_chart_set_series_color(s_chart, s_ser_b, channel_color(d->ch_b));
    }
    for (int k = 0; k < 3; k++) {
        lv_obj_set_style_text_color(s_axis_a[k], channel_color(d->ch_a), 0); /* 刻度文字着色 */
        lv_obj_set_style_text_color(s_axis_b[k], channel_color(d->ch_b), 0);
        if (d->has_b) {
            lv_obj_set_hidden(s_axis_b[k], false);
        } else {
            lv_obj_set_hidden(s_axis_b[k], true);
        }
    }
}

/* ---- 滑板动画 ----
 * LVGL 动画用法：lv_anim_init 建模板 → set_var 指定目标对象 → set_values
 * 给起止值 → set_exec_cb 指定"每帧怎么改属性" → lv_anim_start 启动。
 * set_completed_cb 在动画跑完时回调（用于落定状态/隐藏对象）。 */

/**
 * @brief 动画 exec 回调：把对象 y 坐标设为当前插值 v（滑板升降共用）
 * @param var 目标对象（由 lv_anim_set_var 传入）
 * @param v   本帧插值出的 y 值
 */
static void anim_panel_y(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

/**
 * @brief 滑板升起动画完成回调：置"菜单已打开"标志
 * @param a 动画描述符（本函数不用，故 (void) 忽略）
 */
static void panel_open_done(lv_anim_t *a)
{
    (void)a;
    s_panel_animating = false;
    s_menu_open = true;
}

/**
 * @brief 滑板沉底动画完成回调：隐藏滑板对象并清"菜单打开"标志
 * @param a 动画描述符（本函数不用，故 (void) 忽略）
 */
static void panel_close_done(lv_anim_t *a)
{
    (void)a;
    lv_obj_set_hidden(s_panel, true);
    s_panel_animating = false;
    s_menu_open = false;
}

/**
 * @brief 打开设置滑板（自屏幕底部升起，220ms ease-out）
 *
 * 无参数无返回值。已在打开/动画中则直接返回（防重入）。
 */
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
    lv_anim_init(&anim);                      /* 初始化动画描述符（清零/默认值） */
    lv_anim_set_var(&anim, s_panel);          /* 动画目标：滑板对象 */
    lv_anim_set_values(&anim, g.h, g.h - PANEL_H); /* 自底部升起（横240/竖284） */
    lv_anim_set_duration(&anim, 220);         /* 动画时长 220ms */
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out); /* 缓动曲线：先快后慢 */
    lv_anim_set_exec_cb(&anim, anim_panel_y); /* 每帧执行：改 y 坐标 */
    lv_anim_set_completed_cb(&anim, panel_open_done); /* 跑完回调 */
    lv_anim_start(&anim);                     /* 启动动画（进 LVGL 动画链表） */
}

/**
 * @brief 关闭设置滑板（沉回屏外，180ms ease-in）
 *
 * @param apply true=把 roller 选择落盘到页面状态并刷新头部（"确定"）；
 *              false=放弃本次修改（"退出"），页面保持原设置
 */
void screen_wave_close_menu(bool apply)
{
    if (s_panel == NULL || (!s_menu_open && !s_panel_animating)) {
        /* 滑板不存在或本就未开：只做状态复位（含"动画中被关"的边界情况） */
        s_menu_open = false;
        s_panel_animating = false;
        if (s_panel != NULL) {
            lv_obj_set_hidden(s_panel, true);
        }
        return;
    }
    if (s_menu_open && apply) {
        /* 只有"菜单已完全打开"且要求确定时才落盘——升起途中就关＝放弃修改 */
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

/**
 * @brief 查询设置滑板是否处于打开态
 * @return true=已打开（供 ui_nav 把按键路由到本页菜单）
 */
bool screen_wave_menu_is_open(void)
{
    return s_menu_open;
}

/**
 * @brief 设置滑板打开时的按键路由（K1=BOOT、K2=USER 两键）
 *
 * 单击＝当前焦点 roller 上下换项（BOOT 上、USER 下，环形）；
 * 双击 USER＝切换焦点到另一个 roller；长按＝关菜单（USER 长按顺带"确定"）。
 *
 * @param id 按键 ID（BUTTON_ID_BOOT / BUTTON_ID_USER）
 * @param ev 按键事件（单击/双击/长按起始等）
 * @return true=事件已消费（ui_nav 不再翻页）
 */
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
            sel = (sel - 1 + cnt) % cnt; /* 上一项，环形（-1 越界回末项） */
        } else {
            sel = (sel + 1) % cnt;       /* 下一项，环形 */
        }
        /* LV_ANIM_ON：换项带滚动动画；OFF 则瞬间跳 */
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

/**
 * @brief 波形页滑动处理：上滑出设置滑板 / 下滑关闭（由 ui_nav indev 级滑动识别调用）
 *
 * 三页循环滑动中，竖向滑动不归 tileview（它只管横滑翻页），而是经
 * indev_swipe_event_cb 的「按下记起点、抬起算总位移」判定后回调到本函数。
 *
 * @param dir 滑动方向（LV_DIR_TOP=上滑 / LV_DIR_BOTTOM=下滑）
 */
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
/** 快速选择：把第 k 小的元素放到 a[k]，左侧 ≤、右侧 ≥（原地，不保证整体有序）
 *
 *  与快速排序同源（挖坑-填数 partition），区别是每次只沿 k 所在的一侧
 *  递归，平均 O(n)——比 qsort 全排序 O(n log n) 便宜，量程计算每帧都跑。 */
static void select_kth(float *a, int n, int k)
{
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = a[lo + (hi - lo) / 2]; /* 取中点元素当基准（防有序数组退化） */
        int i = lo, j = hi;
        /* 分区：i 从左找 ≥ pivot、j 从右找 ≤ pivot，两者相遇即本轮分区完成 */
        while (i <= j) {
            while (a[i] < pivot) {
                i++;
            }
            while (a[j] > pivot) {
                j--;
            }
            if (i <= j) {
                float t = a[i]; /* 双指针交换逆序对 */
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        if (k <= j) {
            hi = j;      /* k 在左半段，收缩右界 */
        } else if (k >= i) {
            lo = i;      /* k 在右半段，收缩左界 */
        } else {
            return; /* pivot 就位 */
        }
    }
}

/**
 * @brief 稳健量程：过滤 NaN 后取 2%~98% 分位的 [lo, hi]
 *
 * @param buf 输入样本数组（可含 NaN，会被跳过）
 * @param n   样本个数
 * @param lo  [out] 下界（2% 分位）
 * @param hi  [out] 上界（98% 分位）
 * @return true=成功 / false=无有效样本
 * @note 内部拷贝到 tmp 上做两次快速选择，不破坏调用方的 s_buf_* 原数据。
 */
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
    /* 分位索引：2% 与 98% 处的顺序统计量下标（按排序后名次换算） */
    int i0 = (int)((float)(m - 1) * 0.02f);
    int i1 = (int)((float)(m - 1) * 0.98f);
    if (i1 <= i0) {
        i0 = 0;      /* 样本太少（<50 个）时退化为全范围，保证不越界 */
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
        /* 留白按中值 1% + 微量，保证量程跨度非零且不夸张 */
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
 *
 * @param n 参与量程统计的样本数（= CHART_MAX_POINTS，含 NaN 空槽会被跳过）
 */

/** 收缩防抖：连续满足收缩条件的帧数阈值（实测 3~5 帧较稳） */
#define RANGE_SHRINK_FRAMES 4

static void update_axis_range(int n)
{
    const mode_desc_t *d = &s_mode_desc[s_mode];
    float a_min, a_max, b_min, b_max;
    /* 稳健分位：主轴用平滑前原值（量程该反映真实峰谷，不被平滑抹掉） */
    if (!robust_range(s_buf_a_raw, n, &a_min, &a_max)) {
        return;
    }
    bool has_b = d->has_b && robust_range(s_buf_b, n, &b_min, &b_max);
    /* 副轴无数据（has_b=false）时 b_min/b_max 未初始化，下面只在 has_b 分支使用 */

    if (!s_range_valid) {
        /* 首帧/模式切换后：直接采用当前分位范围作为量程基准 */
        s_range_a_min = a_min;
        s_range_a_max = a_max;
        s_range_b_min = b_min;
        s_range_b_max = b_max;
        s_range_valid = true;
        s_shrink_cnt_a = 0;
        s_shrink_cnt_b = 0;
    } else {
        /* a 通道：扩程加滞回余量（数据超出当前量程 3% 才扩，防单点噪声
         * 触发扩程跳动）；缩程需连续帧确认（防抖）。
         * 判定基于"分位范围"而非原始 min/max——单个坏点再尖也进不了 2% 分位。 */
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

    /* 轴量程与刻度统一加留白：波峰波谷不贴 chart 边框（贴边极难观测）。
     * 2026-10 用户反馈 5% 仍偏紧，提至 12%——峰值落在图面约 88% 处，
     * 上下各留出清晰的呼吸空间（示波器惯例：信号占中间约 3/4 高度）。
     * chart range 与刻度始终一致，峰谷不会顶框。 */
    const float pad_a = (s_range_a_max - s_range_a_min) * WAVE_RANGE_PAD_FRAC;
    const float pad_b = (s_range_b_max - s_range_b_min) * WAVE_RANGE_PAD_FRAC;

    /* 量化量程：每格取 1-2-5 标准格值（电压 ≥1V/格、电流 ≥0.1A/格），
     * 固定 4 格（格线数固定），下边界锚定格值整数倍、上边界 = 下边界+4×格值，
     * 使 5 条格线全部落在整格值整数倍上——读数可直接按格数心算
     * （如 1V、2V、5V / 0.1A、0.5A、1A）。
     * ⚠ floorf(x/div) 有浮点误差（17.0/0.1→169.9999→16.9），加 1e-3 抵消。 */
    float range_lo_a = s_range_a_min - pad_a, range_hi_a = s_range_a_max + pad_a;
    float range_lo_b = s_range_b_min - pad_b, range_hi_b = s_range_b_max + pad_b;

    const bool a_is_volt = (d->ch_a == 0);
    const float min_div_a = a_is_volt ? WAVE_MIN_DIV_V : WAVE_MIN_DIV_A;
    const float span_a = range_hi_a - range_lo_a;
    /* 目标格值 = 跨度/格数，但至少 WAVE_Y_DIVS×min_div（宁可放大格值也不许过细） */
    const float div_a = wave_nice_div(fmaxf(span_a, WAVE_Y_DIVS * min_div_a) / (float)WAVE_Y_DIVS,
                                      a_is_volt);
    {
        /* 以跨度中心为锚：先算"下边界该落在第几格"，floorf 取整后回乘格值——
         * 下边界因此是格值的整数倍，中间 3 条格线也全部落在整格值上 */
        const float mid = (range_lo_a + range_hi_a) * 0.5f;
        const float lo_units = floorf((mid - 0.5f * (float)WAVE_Y_DIVS * div_a) / div_a + 1e-3f);
        range_lo_a = lo_units * div_a;
        range_hi_a = range_lo_a + (float)WAVE_Y_DIVS * div_a;
    }

    if (d->has_b) {
        /* 副轴量化逻辑与主轴完全相同（1-2-5 格值 + floorf 锚定），只是基值
         * 按副轴通道选 1V/0.1A */
        const bool b_is_volt = (d->ch_b == 0);
        const float min_div_b = b_is_volt ? WAVE_MIN_DIV_V : WAVE_MIN_DIV_A;
        const float span_b = range_hi_b - range_lo_b;
        const float div_b = wave_nice_div(fmaxf(span_b, WAVE_Y_DIVS * min_div_b) / (float)WAVE_Y_DIVS,
                                          b_is_volt);
        const float mid = (range_lo_b + range_hi_b) * 0.5f;
        const float lo_units = floorf((mid - 0.5f * (float)WAVE_Y_DIVS * div_b) / div_b + 1e-3f);
        range_lo_b = lo_units * div_b;
        range_hi_b = range_lo_b + (float)WAVE_Y_DIVS * div_b;
    }
    /* lv_chart_set_axis_range：设置图表指定轴的数值范围（比 lv_chart_set_range
     * 更精确——可分别指定主/副 Y 轴）。LVGL 图表内部存整数，故物理值 ×100 后
     * 传入（CHART_VALUE_SCALE），绘图时再按此范围线性映射到像素高度。 */
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y,
                            (int32_t)(range_lo_a * CHART_VALUE_SCALE),
                            (int32_t)(range_hi_a * CHART_VALUE_SCALE));
    if (d->has_b) {
        lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_SECONDARY_Y,
                                (int32_t)(range_lo_b * CHART_VALUE_SCALE),
                                (int32_t)(range_hi_b * CHART_VALUE_SCALE));
    }

    /* 轴刻度（上/中/下）：与 chart range 一致（含留白与量化后的范围）。
     * 取上/中/下 3 个值：上=range_hi、中=两者均值、下=range_lo。 */
    char txt[16];
    const float avals[3] = { range_hi_a,
                             (range_lo_a + range_hi_a) * 0.5f,
                             range_lo_a };
    const float bvals[3] = { range_hi_b,
                             (range_lo_b + range_hi_b) * 0.5f,
                             range_lo_b };
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
 *
 * @param n 样本数（= CHART_MAX_POINTS）
 * @param d 当前波形模式描述（判断有无副轴；当前实现只画主轴趋势）
 * @note 仅当 WAVE_DECOR_ENABLE=1 时被 screen_wave_refresh 调用（现默认关闭）。
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
    /* 显著更极端阈值：超出当前量程跨度 2% 才换锚（否则视为同一极值事件） */
    const float ext_thresh = (s_range_a_max - s_range_a_min) * 0.02f + 1e-6f;
    for (int i = 0; i < n; i++) {
        const float v = s_buf_a_raw[i];
        if (!isfinite(v)) {
            continue; /* NaN 空槽：无数据不参与统计 */
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

    /* 均值参考线（水平 1px 浅色，跨图面宽）：按主轴量程（含 12% 留白，
     * 与 WAVE_RANGE_PAD_FRAC 一致）换算 y。
     * ⚠ 必须与 update_axis_range 中 chart_set_axis_range 的留白范围一致，
     * 否则均值线位置与曲线实际位置不匹配。 */
    int32_t y_avg;
    {
        const float pad = (s_range_a_max - s_range_a_min) * WAVE_RANGE_PAD_FRAC;
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
     * 不用曲线同色（绿点压绿线几乎不可见）；深色在任何通道色上都清晰。
     * lv_chart_get_point_pos_by_id：取第 id 个数据点在 chart 内容区的像素
     * 坐标（自动完成"数值→像素"换算与 padding 偏移），免得自己按量程反算。 */
    if (s_dot_min != NULL && s_ser_a != NULL) {
        lv_point_t pm;
        lv_chart_get_point_pos_by_id(s_chart, s_ser_a, (uint32_t)i_min_dot, &pm);
        /* -2 使 4px 圆心对准数据点（圆点左上角 = 中心 - 半径） */
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

/**
 * @brief 波形页主刷新：拉数据 → 平滑 → 量程 → 写入 chart → 刷新数据行
 *
 * 每帧由 UI 任务周期调用（约 100ms 级）。核心契约是"时间槽一比一映射"：
 * 缓冲区第 j 槽 = 屏幕第 j 列，右端 = 现在。
 */
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
     * 【Step2 降噪】副轴（常为电流，空载噪声大）同样平滑——原先只平滑主轴。
     * 先备份原值到 s_buf_*_raw：量程统计与趋势装饰都基于原值（平滑会削峰）。 */
    settings_t scfg;
    bool smooth = (settings_get(&scfg) == ESP_OK && scfg.wave_smooth);
    for (int i = 0; i < CHART_MAX_POINTS; i++) {
        s_buf_a[i] = s_buf_a_raw[i];
        s_buf_b_raw[i] = s_buf_b[i];
    }
    if (smooth) {
        for (int i = 1; i < CHART_MAX_POINTS - 1; i++) {
            /* 3 点均值：相邻三槽都有效才平滑，任一为 NaN 则保持原值（不跨断线） */
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

    /* 一比一映射到 chart 槽位：j → 第 j 列；NaN → LV_CHART_POINT_NONE（断线）。
     * lv_chart_set_series_value_by_id：按槽位 id 直接覆写该点的值（整帧刷新用；
     * 相邻追加点用 lv_chart_set_next_value）。LV_CHART_POINT_NONE 是 LVGL 内置的
     * "无数据"哨兵值，渲染时该点断线不画，正好对应本页的 NaN 空槽语义。 */
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
    /* 批量写完槽位后手动触发重绘：lv_chart_refresh 让 chart 立即按最新
     * 数据重画曲线（否则要等下个刷新周期才见效果）。 */
    lv_chart_refresh(s_chart);
#if WAVE_DECOR_ENABLE
    trend_update(CHART_MAX_POINTS, d); /* 均值线 + min/max 极值点 */
#else
    (void)d;
#endif

    /* 底部「实时/平均」数据行：定宽列布局（每列数值右对齐 + 单位固定右端，
     * 数值正负跳动只在本列内生长，不影响其他列——观测不晃眼）。
     * 跳过无数据的 NaN 空槽。 */
    float a_last = 0.0f, b_last = 0.0f, a_avg = 0.0f, b_avg = 0.0f;
    int cnt = 0;
    /* 用 double 累加：480 个 float 累加用 float 会积累舍入误差 */
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
        return; /* 整窗无有效数据：数据行保持上一帧内容，不刷 "--" */
    }
    a_avg = (float)(sa / cnt);
    b_avg = d->has_b ? (float)(sb / cnt) : 0.0f;

    /* 列数变化才重排版（模式切换时），平时只更新数值文本。
     * VI 模式 3 列（V/I/派生 W），VP、IP 模式 2 列，P/E 模式 1 列。 */
    int ncol;
    switch (s_mode) {
    case PM_WAVE_MODE_VI: ncol = 3; break;
    case PM_WAVE_MODE_VP:
    case PM_WAVE_MODE_IP: ncol = 2; break;
    default: ncol = 1; break;
    }
    if (ncol != s_data_ncol) {
        s_data_ncol = ncol;
        const wave_geom_t g_layout = wave_geom();
        layout_data_cols(&g_layout);
    }

    /* 列内容：列 0/1/2 = a 通道值 / b 通道值 / 派生功率。
     * 数值按通道单位定小数位，负号自然右对齐、末位稳定不跳。
     * VI 模式第 3 列是瞬时功率＝电压×电流（UI 侧派生，非通道直读）。 */
    float col_v[WAVE_DATA_COLS];
    int col_ch[WAVE_DATA_COLS];
    switch (s_mode) {
    case PM_WAVE_MODE_VI:
        col_v[0] = a_last; col_ch[0] = d->ch_a;
        col_v[1] = b_last; col_ch[1] = d->ch_b;
        col_v[2] = a_last * b_last; col_ch[2] = 2;
        break;
    case PM_WAVE_MODE_VP:
    case PM_WAVE_MODE_IP:
        col_v[0] = a_last; col_ch[0] = d->ch_a;
        col_v[1] = b_last; col_ch[1] = d->ch_b;
        col_v[2] = 0; col_ch[2] = 2;
        break;
    case PM_WAVE_MODE_P:
    default:
        col_v[0] = a_last; col_ch[0] = d->ch_a;
        col_v[1] = 0; col_ch[1] = -1;
        col_v[2] = 0; col_ch[2] = -1;
        break;
    }
    float col_av[WAVE_DATA_COLS];
    switch (s_mode) {
    case PM_WAVE_MODE_VI:
        col_av[0] = a_avg; col_av[1] = b_avg; col_av[2] = a_avg * b_avg;
        break;
    case PM_WAVE_MODE_VP:
    case PM_WAVE_MODE_IP:
        col_av[0] = a_avg; col_av[1] = b_avg; col_av[2] = 0;
        break;
    default:
        col_av[0] = a_avg; col_av[1] = 0; col_av[2] = 0;
        break;
    }

    char vt[16];
    for (int c = 0; c < WAVE_DATA_COLS; c++) {
        if (c >= ncol) {
            break; /* 未用列隐藏 */
        }
        /* lv_label_set_text：给 label 赋新文本，LVGL 自动重算尺寸并重绘 */
        format_axis_val(vt, sizeof(vt), col_ch[c], col_v[c]);
        lv_label_set_text(s_rt_val[c], vt); /* 实时行数值 */
        format_axis_val(vt, sizeof(vt), col_ch[c], col_av[c]);
        lv_label_set_text(s_av_val[c], vt); /* 平均行数值 */
        /* 单位固定列右端（模式切换时随之更新） */
        static const char *const unit_txt[4] = { "V", "A", "W", "Wh" };
        const char *u = (col_ch[c] >= 0 && col_ch[c] < 4) ? unit_txt[col_ch[c]] : "";
        lv_label_set_text(s_rt_unit[c], u);
        lv_label_set_text(s_av_unit[c], u);
    }
}

/**
 * @brief chart 尺寸确定后摆放 6 个轴刻度（浮于曲线之上，左右贴边）
 *
 * 2026-10 改：波形铺满整宽，刻度不再占 padding 留白（原先两侧各让出
 * 66px，曲线被挤在中间很不协调）。刻度改贴左右边缘、加半透明白底，
 * 压在曲线之上仍可读。
 *
 * @param e 事件对象（绑定在 chart 的 LV_EVENT_SIZE_CHANGED 上，
 *          chart 尺寸变化时由 LVGL 自动调用）
 * @note 用 lv_event_get_current_target 取绑定事件的对象（chart 本身），
 *       而非 lv_event_get_target（本回调中两者相同，但语义上前者更准）。
 */
static void chart_size_event_cb(lv_event_t *e)
{
    lv_obj_t *chart = lv_event_get_current_target(e);
    /* lv_obj_get_content_width/height：取对象内容区尺寸（不含 border/padding），
     * 刻度定位基于曲线实际绘制区域，故用 content 尺寸而非对象总宽高。 */
    lv_coord_t w = lv_obj_get_content_width(chart);
    lv_coord_t h = lv_obj_get_content_height(chart);
    for (int k = 0; k < 3; k++) {
        lv_coord_t y = (k == 0) ? -3 : (k == 1) ? (h - 14) / 2 : (h - 11);
        lv_obj_set_pos(s_axis_a[k], 0, y);            /* 左刻度贴左缘 */
        lv_obj_set_pos(s_axis_b[k], w - AXIS_LABEL_W, y); /* 右刻度贴右缘 */
    }
}

/**
 * @brief 曲线平滑开关回调（LV_EVENT_VALUE_CHANGED）
 *
 * 读取开关状态写入 settings 并立即持久化（下一次刷新即用新设置）。
 * @param e 事件对象（携带开关对象）
 */
static void smooth_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    /* lv_obj_has_state：查询对象是否处于某状态（开关选中＝LV_STATE_CHECKED） */
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    (void)settings_set_wave_smooth(on ? 1u : 0u);
    (void)settings_save_now();
}

/**
 * @brief 构建底部设置滑板（标题 + 双 roller + 平滑开关 + 两行提示）
 *
 * 滑板挂 **screen 层**（lv_screen_active）而非 tile——旋转重建 tile 时
 * 滑板可复用/单独清理（见 screen_wave_create 的清理逻辑）。
 *
 * @param scr 父级 screen 对象
 * @param g   布局几何（决定两列 roller 宽度）
 */
static void create_panel(lv_obj_t *scr, const wave_geom_t *g)
{
    s_panel = lv_obj_create(scr); /* lv_obj_create：创建基础容器对象（带默认样式） */
    lv_obj_set_size(s_panel, LV_PCT(100), PANEL_H); /* LV_PCT(100)=父级宽度 100% */
    lv_obj_set_pos(s_panel, 0, g->h); /* 初始沉在屏外（横 240 / 竖 284） */
    lv_obj_set_style_bg_color(s_panel, COL_PANEL_BG, 0); /* 背景色（0=默认状态） */
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);   /* 背景不透明度 100% */
    lv_obj_set_style_radius(s_panel, 12, 0); /* 圆角半径 12px */
    lv_obj_set_style_border_color(s_panel, COL_CARD_BD, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_pad_all(s_panel, 10, 0); /* 四边统一内边距 10px */
    lv_obj_set_style_pad_hor(s_panel, 16, 0); /* 面板内也避开圆角（左右内边距 16） */
    /* lv_obj_set_flex_flow：把容器设为弹性布局（COLUMN=子对象纵向排列） */
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    /* lv_obj_set_flex_align：主轴/交叉轴/整体对齐方式（START/CENTER/CENTER） */
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_panel, 4, 0); /* 弹性布局子项行间距 4px */
    lv_obj_set_hidden(s_panel, true);        /* 初始隐藏（动画时才显示） */
    lv_obj_set_scrollable(s_panel, false);   /* 滑板自身不可滚动 */

    lv_obj_t *title = lv_label_create(s_panel); /* lv_label_create：创建文本标签 */
    lv_label_set_text(title, "波形设置");
    lv_obj_set_style_text_font(title, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    lv_obj_set_style_text_color(title, COL_DARK, 0);

    lv_obj_t *row = lv_obj_create(s_panel);
    lv_obj_remove_style_all(row); /* 清掉默认样式（无边框/无背景的纯布局行） */
    lv_obj_set_size(row, LV_PCT(100), 88);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW); /* 横向弹性排列两个 roller 列 */
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
    s_roller_win = lv_roller_create(col_w); /* lv_roller_create：创建滚轮选择器 */
    /* lv_roller_set_options：设置选项列表，'\n' 分隔各项（LV_ROLLER_MODE_NORMAL=普通模式） */
    lv_roller_set_options(s_roller_win, "1s\n10s\n30s\n60s\n120s\n10min",
                          LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_win, 3); /* 可见行数 3（中间为选中项） */
    lv_roller_set_selected(s_roller_win, (uint16_t)s_window, LV_ANIM_OFF); /* 初始选中当前窗 */
    lv_obj_set_size(s_roller_win, g->col_win_w - 8, 68);
    /* 两阶段触控：初始禁滑动（防下滑关闭菜单时误改），点击选中后才可滑 */
    lv_obj_set_scrollable(s_roller_win, false);
    /* lv_obj_add_event_cb：给对象挂事件回调（LV_EVENT_CLICKED=点击释放时触发） */
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
    /* 5 种波形模式，顺序与 pm_wave_mode_t 枚举一致（P/IP/VP/VI/E） */
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
    lv_obj_t *sw = lv_switch_create(smooth_row); /* lv_switch_create：创建开关控件 */
    /* 与设置页自动旋转开关统一样式：关闭轨道灰、开启轨道品牌蓝、
     * 白色旋钮带 1px 灰边框（避免白底上旋钮"消失"）。
     * LV_STATE_CHECKED=开关开启态、LV_PART_KNOB=旋钮子部件——LVGL 样式可以
     * 按「部件+状态」分别设置，同一个对象不同状态/子件各配各的外观。 */
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

/**
 * @brief 构建波形页（tile 重建＝横竖屏旋转后 ui_nav 会调用）
 *
 * 先清理旧滑板/置空全部对象指针，再按 wave_geom() 逐层搭 UI。
 * @param tile lv_tileview 的波形页 tile（本页所有内容的父级）
 */
void screen_wave_create(lv_obj_t *tile)
{
    wave_geom_t g = wave_geom();

    /* 【U2 修复】旋转重建必须复位 roller 两阶段触控状态 s_roller_edit：
     * 它是 int8_t（-1=无编辑 / 0=时间窗 / 1=波形模式），重建只复位
     * s_menu_open/s_panel_animating 会残留旧值——重建后若仍为 0/1，
     * roller_clicked_cb 会把"首次点击选中"误判成"再点一次确认退出"，
     * 用户点一下 roller 直接退出编辑态，滑不动选项。
     * ⚠ 必须在删除 s_panel **之前**调用：两个 roller 是滑板的子孙对象，
     * 先删滑板会让 roller_edit_cancel() 里的 set_scrollable 踩到悬空指针。 */
    roller_edit_cancel();
    if (s_panel != NULL) {
        /* ---- 旋转重建清理：底部滑板挂在 screen 层（不随 tile 删除），
         * 必须先停掉位移动画再删对象，否则动画回调会访问已释放的 panel。
         * lv_anim_del(obj, exec_cb)：按 exec 回调删除该对象上的动画 ---- */
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
    s_data_ncol = 0; /* 下次重建时强制重新排版 */
    s_lbl_time_m = NULL;
    s_rt_prefix = NULL;
    s_av_prefix = NULL;
    for (int c = 0; c < WAVE_DATA_COLS; c++) {
        s_rt_val[c] = NULL;
        s_av_val[c] = NULL;
        s_rt_unit[c] = NULL;
        s_av_unit[c] = NULL;
    }
    s_roller_win = NULL;
    s_roller_mode = NULL;
    for (int k = 0; k < 3; k++) {
        s_axis_a[k] = NULL;
        s_axis_b[k] = NULL;
    }
    /* B20 波形持久化：从 settings 恢复时间窗/波形模式初值。
     * 与旧"旋转重建保留静态值"策略等价——用户每次确定切换都已实时写回
     * settings 内存缓存（apply_roller_selection），重建时读回的即是最新值。
     * 越界值回默认：10s 窗 / 电压+电流模式（与旧版默认一致）。量程滞回状态仍保留不重建。 */
    settings_t wcfg;
    if (settings_get(&wcfg) == ESP_OK) {
        s_window = (wcfg.wave_window < (uint8_t)PM_WAVE_WINDOW_MAX)
                       ? (pm_wave_window_t)wcfg.wave_window
                       : PM_WAVE_10S;
        s_mode = (wcfg.wave_mode < (uint8_t)PM_WAVE_MODE_MAX)
                     ? (pm_wave_mode_t)wcfg.wave_mode
                     : PM_WAVE_MODE_VI;
    }

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

    /* 图表：LVGL 的 lv_chart 控件——多系列折线/柱状图，自带格线与坐标映射 */
    s_chart = lv_chart_create(tile);
    lv_obj_set_size(s_chart, g.w - 2 * SAFE_X, g.chart_h);
    lv_obj_set_pos(s_chart, SAFE_X, g.chart_y);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);      /* 图表类型：折线图 */
    lv_chart_set_point_count(s_chart, CHART_MAX_POINTS); /* 每系列的点（槽）数 480 */
    /* 每帧用 set_series_value_by_id 整帧重写全部槽位（时间槽映射，不是
     * "逐点追加"），update_mode 只影响 set_next_value，这里无需设置。 */
    /* lv_chart_set_div_line_count(chart, x, y)：图表内部格线数量（横线数/纵线数）。
     * 注意参数是「线数」不是「格数」：5 条横线 → 4 格（即 WAVE_Y_DIVS）。
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

    /* lv_chart_add_series：给图表加一条曲线（颜色 + 绑定主/副 Y 轴）。
     * 主轴=左刻度、副轴=右刻度，双轴曲线各按各的量程映射到同一图面高度。 */
    s_ser_a = lv_chart_add_series(s_chart, COL_V, LV_CHART_AXIS_PRIMARY_Y);
    s_ser_b = lv_chart_add_series(s_chart, COL_I, LV_CHART_AXIS_SECONDARY_Y);
    /* lv_chart_set_all_values：把整条系列所有槽位一次性置为某值——
     * 开局置 LV_CHART_POINT_NONE 表示"全空"，避免显示残留垃圾值 */
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

    /* 实时 / 平均：定宽列标签组（行首"实"/"均" + 每列 定宽数值 + 定宽单位）。
     * 数值右对齐定宽框、单位固定列右端——数字正负跳动时列间不互挤。
     * 字体分级：数值 16px（主读数）、前缀 14px、单位 12px（小、不抢眼）。
     * 行高按数值字体 16px 计算，单位垂直居中（避免飘在右上角像上标）。
     * 建标签时位置先给"默认值"，最终位置由 layout_data_cols 统一摆放。 */
    const lv_font_t *f16v = lvgl_port_get_font(LVGL_PORT_FONT_16);
    const lv_font_t *f14 = lvgl_port_get_font(LVGL_PORT_FONT_14);
    const lv_font_t *f12u = lvgl_port_get_font(LVGL_PORT_FONT_12);
    const lv_coord_t line_h = (lv_coord_t)(f16v->line_height + 2);
    static const char *const prefix_txt[2] = { "实", "均" };
    const lv_color_t prefix_col[2] = { COL_DARK, COL_GRAY };
    lv_obj_t **prefix_obj[2] = { &s_rt_prefix, &s_av_prefix };
    for (int row = 0; row < 2; row++) {
        *prefix_obj[row] = lv_label_create(tile);
        lv_label_set_text(*prefix_obj[row], prefix_txt[row]);
        lv_obj_set_style_text_font(*prefix_obj[row], f14, 0);
        lv_obj_set_style_text_color(*prefix_obj[row], prefix_col[row], 0);
    }
    lv_obj_t **vals[2] = { s_rt_val, s_av_val };
    lv_obj_t **units[2] = { s_rt_unit, s_av_unit };
    for (int row = 0; row < 2; row++) {
        for (int c = 0; c < WAVE_DATA_COLS; c++) {
            vals[row][c] = lv_label_create(tile);
            lv_label_set_text(vals[row][c], "--");
            lv_obj_set_style_text_font(vals[row][c], f16v, 0);
            lv_obj_set_style_text_color(vals[row][c], prefix_col[row], 0);
            lv_obj_set_width(vals[row][c], 60);
            lv_obj_set_style_text_align(vals[row][c], LV_TEXT_ALIGN_RIGHT, 0);

            units[row][c] = lv_label_create(tile);
            lv_label_set_text(units[row][c], "");
            lv_obj_set_style_text_font(units[row][c], f12u, 0);
            lv_obj_set_style_text_color(units[row][c], COL_GRAY, 0);
        }
    }
    s_data_ncol = 0; /* 强制下次刷新时 layout_data_cols 排版 */
    layout_data_cols(&g);

    update_header();
    create_panel(lv_screen_active(), &g);
}

/**
 * @brief 取当前时间窗（供 pm_wave_copy 之外的调用方查询页面状态）
 * @return 当前时间窗枚举
 */
pm_wave_window_t screen_wave_get_window(void)
{
    return s_window;
}

/**
 * @brief 取当前波形模式
 * @return 当前波形模式枚举
 */
pm_wave_mode_t screen_wave_get_mode(void)
{
    return s_mode;
}
