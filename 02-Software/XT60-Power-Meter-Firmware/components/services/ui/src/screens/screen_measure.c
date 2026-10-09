/**
 * @file screen_measure.c
 * @brief 测量数据页：四行满宽彩色条（参考桌面 USB 表的经典版式）
 *
 * 版式（284×240，面板四角圆角 R≈22，故内容左右各内缩 16px、首末行避开圆角）：
 *   ┌ x=16 ────────────────── x=268 ┐
 *   │ 00:34:38            (WiFi) 88%│ 状态栏（由 ui_nav 共享浮层绘制）
 *   │┌────────────────────────────┐ │ y=22  高 46
 *   ││ 19.97670              V    │ │ 电压：绿色底
 *   │└────────────────────────────┘ │
 *   │┌────────────────────────────┐ │ y=70
 *   ││ 0.01886              ▶A    │ │ 电流：蓝色底（箭头指示方向）
 *   │└────────────────────────────┘ │
 *   │┌────────────────────────────┐ │ y=118
 *   ││ 0.37683              W     │ │ 功率：红色底
 *   │└────────────────────────────┘ │
 *   │┌────────────────────────────┐ │ y=166
 *   ││ 0.11800              Wh    │ │ 电能：紫色底
 *   │└────────────────────────────┘ │
 *   │ 32.5℃ 30%RH                   │ 环境小字（指示点之上）
 *   └───────────────────────────────┘
 *
 * 设计说明：
 *   · 白字 + 彩色底 + 5 位小数：与同类产品（RYC/UM34）一致，远看也能读数；
 *   · 单位小字贴右边缘，与数字拉开，避免"数字变了撞到单位"；
 *   · 电流带方向箭头：横屏朝右为正向（▶），负向为 ◀；
 *   · 数字字号 26px Bold + 单位 12px：在 46px 行高内既醒目又不溢出。
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

/** 内容左右安全边距（避开面板圆角） */
#define SAFE_X 16
#define ROW_H 42
#define ROW_GAP 3
#define ROW_TOP 24

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
    lv_obj_set_size(row, 284 - 2 * SAFE_X, ROW_H);
    lv_obj_set_pos(row, SAFE_X, y);
    lv_obj_set_style_bg_color(row, bg, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    m->row = row;

    /* 左：大数字（白字，5 位小数固定） */
    m->value = lv_label_create(row);
    lv_label_set_text(m->value, "0.00000");
    lv_obj_set_style_text_font(m->value, lvgl_port_get_font_bold(LVGL_PORT_FONT_24), 0);
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

    s_lbl_env = lv_label_create(tile);
    lv_label_set_text(s_lbl_env, "--.-℃  --%RH");
    lv_obj_set_style_text_font(s_lbl_env, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(s_lbl_env, COL_ENV, 0);
    lv_obj_set_pos(s_lbl_env, SAFE_X, ROW_TOP + 4 * (ROW_H + ROW_GAP) + 2);
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

    char env[40];
    snprintf(env, sizeof(env), "%.1f℃  %.0f%%RH", (double)snap->temperature_c,
             (double)snap->humidity_pct);
    lv_label_set_text(s_lbl_env, env);
}
