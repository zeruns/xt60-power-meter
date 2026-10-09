/**
 * @file screen_measure.c
 * @brief 测量数据页：四行满宽彩色条（参考桌面 USB 表的经典版式）
 *
 * 版式（284×240，面板四角圆角 R≈22）：
 *   ┌ x=6 ───────────────────── x=277 ┐
 *   │ 00:34:38    •••    (WiFi) 88%  │ 状态栏（ui_nav 共享浮层，22px 高）
 *   │┌──────────────────────────────┐│ y=22  高 44
 *   ││ 19.97670                    V││ 电压：绿底
 *   │└──────────────────────────────┘│
 *   │┌──────────────────────────────┐│ y=68
 *   ││ 0.01886                   ▶A ││ 电流：蓝底（箭头示方向）
 *   │└──────────────────────────────┘│
 *   │┌──────────────────────────────┐│ y=114
 *   ││ 0.37683                     W││ 功率：红底
 *   │└──────────────────────────────┘│
 *   │┌──────────────────────────────┐│ y=160
 *   ││ 0.11800                    Wh││ 电能：紫底
 *   │└──────────────────────────────┘│
 *   │          32.5℃  30%RH          │ 环境温湿度（底部居中）
 *   └────────────────────────────────┘
 *
 * 设计说明：
 *   · 数字用 32px Bold 白字填满 44px 行高（"贴近容器大小"，远看可读）；
 *   · 行左右只留 6px：四行都落在 y=22~204 的垂直中段，该区域内面板
 *     圆角不侵入，因此可以像成品表一样做到近满宽；
 *   · 单位小字（Bold 14）贴右内边距，电流行单位左侧再放方向箭头；
 *   · 5 位小数固定宽度：最坏 11 字符（99999.99999）在 Bold 32 下约 200px，
 *     小于可用 211px，不会撞到单位。
 */
#include "screen_measure.h"

#include <math.h>
#include <stdio.h>

#include "lvgl.h"
#include "lvgl_port.h"

/* 四行配色（白字底）：与波形页同数字项的颜色语义保持一致 */
#define COL_V  lv_color_hex(0x1E9E4F) /* 电压 绿 */
#define COL_I  lv_color_hex(0x1B6FD6) /* 电流 蓝 */
#define COL_P  lv_color_hex(0xD0392B) /* 功率 红 */
#define COL_E  lv_color_hex(0x7D3CB5) /* 电能 紫 */
#define COL_ENV lv_color_hex(0x6B7280)

/** 内容左右边距：四行都在垂直中段，圆角不侵入，故可做到近满宽 */
#define SAFE_X 6
#define ROW_W (284 - 2 * SAFE_X)
#define ROW_H 44
#define ROW_GAP 2
#define ROW_TOP 22
/** 行内左右内边距 */
#define ROW_PAD 8
/** 环境温湿度文字的垂直位置（底部居中，见 ui_nav 已把页码点上移） */
#define ENV_Y 214

typedef struct {
    lv_obj_t *row;   /**< 彩色条本体 */
    lv_obj_t *value; /**< 大数字（白字） */
    lv_obj_t *unit;  /**< 单位小字 */
    lv_obj_t *arrow; /**< 方向箭头（仅电流用） */
} metric_t;

static metric_t s_v, s_i, s_p, s_e;
static lv_obj_t *s_lbl_env;

/**
 * @brief 创建一行数据条
 * @param y 该行顶部 y 坐标
 */
static void create_row(lv_obj_t *parent, int y, lv_color_t bg, metric_t *m, bool with_arrow)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_set_pos(row, SAFE_X, y);
    lv_obj_set_style_bg_color(row, bg, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_hor(row, ROW_PAD, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    m->row = row;

    /* 左：大数字（白字，5 位小数固定），32px Bold 填满 44px 行高 */
    m->value = lv_label_create(row);
    lv_label_set_text(m->value, "0.00000");
    lv_obj_set_style_text_font(m->value, lvgl_port_get_font_bold(LVGL_PORT_FONT_32), 0);
    lv_obj_set_style_text_color(m->value, lv_color_white(), 0);

    /* 右：箭头（可选）+ 单位 */
    lv_obj_t *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(right, false);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 3, 0);

    if (with_arrow) {
        m->arrow = lv_label_create(right);
        lv_label_set_text(m->arrow, "▶");
        lv_obj_set_style_text_font(m->arrow, lvgl_port_get_font(LVGL_PORT_FONT_20), 0);
        lv_obj_set_style_text_color(m->arrow, lv_color_white(), 0);
    } else {
        m->arrow = NULL;
    }

    m->unit = lv_label_create(right);
    lv_label_set_text(m->unit, "");
    lv_obj_set_style_text_font(m->unit, lvgl_port_get_font_bold(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(m->unit, lv_color_white(), 0);
}

void screen_measure_create(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(tile, false);
    /* 不用 flex：这里全部用绝对坐标，才能精确控制与圆角的距离 */
    lv_obj_set_style_pad_all(tile, 0, 0);

    create_row(tile, ROW_TOP, COL_V, &s_v, false);
    create_row(tile, ROW_TOP + (ROW_H + ROW_GAP), COL_I, &s_i, true);
    create_row(tile, ROW_TOP + 2 * (ROW_H + ROW_GAP), COL_P, &s_p, false);
    create_row(tile, ROW_TOP + 3 * (ROW_H + ROW_GAP), COL_E, &s_e, false);

    /* 环境温湿度：底部居中（页码点已移到状态栏，底部整条留给它） */
    s_lbl_env = lv_label_create(tile);
    lv_label_set_text(s_lbl_env, "--.-℃  --%RH");
    lv_obj_set_style_text_font(s_lbl_env, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(s_lbl_env, COL_ENV, 0);
    lv_obj_set_width(s_lbl_env, 284);
    lv_obj_set_style_text_align(s_lbl_env, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_env, 0, ENV_Y);
}

/** 固定 5 位小数格式化（NaN/超范围时给出安全文本，避免显示 inf） */
static void fmt5(char *dst, size_t n, float v)
{
    if (!isfinite(v)) {
        snprintf(dst, n, "0.00000");
        return;
    }
    /* 超过 5 位小数能表示的范围时改用科学计数，避免撑破行宽 */
    float a = fabsf(v);
    if (a >= 100000.0f) {
        snprintf(dst, n, "%.3e", (double)v);
    } else {
        snprintf(dst, n, "%.5f", (double)v);
    }
}

void screen_measure_refresh(const data_store_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }

    char buf[24];

    fmt5(buf, sizeof(buf), snap->voltage_v);
    lv_label_set_text(s_v.value, buf);
    lv_label_set_text(s_v.unit, "V");

    fmt5(buf, sizeof(buf), snap->current_a);
    lv_label_set_text(s_i.value, buf);
    lv_label_set_text(s_i.unit, "A");
    if (s_i.arrow != NULL) {
        /* 横屏：正电流向右，负电流向左 */
        lv_label_set_text(s_i.arrow, (snap->current_a < 0.0f) ? "◀" : "▶");
    }

    fmt5(buf, sizeof(buf), snap->power_w);
    lv_label_set_text(s_p.value, buf);
    lv_label_set_text(s_p.unit, "W");

    fmt5(buf, sizeof(buf), snap->energy_wh);
    lv_label_set_text(s_e.value, buf);
    lv_label_set_text(s_e.unit, "Wh");

    /* AHT20 尚未读出有效值时（两路都是 0）显示占位符，而不是误导性的 0 */
    char env[40];
    if (snap->temperature_c == 0.0f && snap->humidity_pct == 0.0f) {
        snprintf(env, sizeof(env), "--.-℃  --%%RH");
    } else {
        snprintf(env, sizeof(env), "%.1f℃  %.0f%%RH", (double)snap->temperature_c,
                 (double)snap->humidity_pct);
    }
    lv_label_set_text(s_lbl_env, env);
}
