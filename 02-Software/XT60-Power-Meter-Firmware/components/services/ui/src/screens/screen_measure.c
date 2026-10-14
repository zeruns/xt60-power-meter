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
 *   · 数字用 36px Bold 白字填满 44px 行高（"贴近容器大小"，远看可读）；
 *     竖屏（90°/270°）多出的垂直空间用于加大到 40px Bold / 54px 行高；
 *   · 行左右只留 6px：四行都落在垂直中段，该区域内面板
 *     圆角不侵入，因此可以像成品表一样做到近满宽；
 *   · 单位小字（Bold 16）贴右内边距，电流行单位左侧再放方向箭头；
 *     箭头方向随屏幕角度自适应（0°→右 / 90°→下 / 180°→左 / 270°→上），
 *     负电流取反向——电流物理流向固定，屏幕转了箭头跟着转；
 *   · 动态小数位（≤9 字符）：|v|<1 用 5 位、<100 用 4 位、<10000 用 2 位；
 *     40px Bold 下最宽约 213px（极端 8 字符 182px），加单位/箭头不撞行宽；
 *   · K2 长按打开"快速菜单"（全程按键可操作：清除电能统计 / 刷新率 1~30Hz）。
 */
#include "screen_measure.h"

#include <math.h>
#include <stdio.h>

#include "lvgl.h"
#include "lvgl_port.h"

#include "buzzer.h"
#include "power_measure.h"
#include "ui.h"

/* 四行配色（白字底）：与波形页同数字项的颜色语义保持一致 */
#define COL_V  lv_color_hex(0x1E9E4F) /* 电压 绿 */
#define COL_I  lv_color_hex(0x1B6FD6) /* 电流 蓝 */
#define COL_P  lv_color_hex(0xD0392B) /* 功率 红 */
#define COL_E  lv_color_hex(0x7D3CB5) /* 电能 紫 */
#define COL_ENV lv_color_hex(0x6B7280)
/* 快速菜单用色（与设置页同语义） */
#define COL_PRIMARY lv_color_hex(0x0066CC)
#define COL_INK     lv_color_hex(0x1A1A1A)
#define COL_CARD    lv_color_hex(0xF7F9FC)
#define COL_BORDER  lv_color_hex(0xE3E7EC)

/** 内容左右边距：四行都在垂直中段，圆角不侵入，故可做到近满宽 */
#define SAFE_X 6
#define ROW_GAP 2
/** 行内左右内边距 */
#define ROW_PAD 8

/**
 * @brief 当前分辨率下的布局几何（横/竖屏各一套）
 *
 * 横屏 284×240 的取值为历史调优值（逐像素保持）；竖屏 240×284 时
 * 多出的垂直空间用于加大行高与数字字号（用户需求：竖屏底部空间大），
 * 环境行跟随，快速菜单的档位按钮收窄以放进 240 宽。
 * 旋转后 ui_nav 会重建本页，create 时重新取几何。
 */
typedef struct {
    int32_t w;            /**< 逻辑宽 */
    int32_t h;            /**< 逻辑高 */
    int32_t row_top;      /**< 第一行顶部 y */
    int32_t row_h;        /**< 数据行高 */
    int32_t row_gap;      /**< 行间距 */
    int32_t env_y;        /**< 环境温湿度行 y */
    int32_t rate_btn_w;   /**< 快速菜单刷新率档位按钮宽 */
    lvgl_port_font_size_t val_font; /**< 大数字字号 */
} measure_geom_t;

static measure_geom_t measure_geom(void)
{
    lv_display_t *d = lv_display_get_default();
    int32_t w = (d != NULL) ? lv_display_get_horizontal_resolution(d) : 284;
    int32_t h = (d != NULL) ? lv_display_get_vertical_resolution(d) : 240;
    measure_geom_t g;
    g.w = w;
    g.h = h;
    if (w >= h) {
        /* 横屏 284×240（0°/180°）：历史调优值，逐像素保持 */
        g.row_top = 22;
        g.row_h = 44;
        g.row_gap = ROW_GAP;
        g.env_y = 214;
        g.rate_btn_w = 48;
        g.val_font = LVGL_PORT_FONT_36;
    } else {
        /* 竖屏 240×284（90°/270°）：行高 44→54、数字 36→40px，
         * 四行占满多出的垂直空间（4×54+3×4=228，y=30..258），
         * 环境行沉到底部 262。40px Bold 下最宽 8 字符≈182px，
         * 加单位/箭头 ≤207px < 行内可用 212px，不溢出。 */
        g.row_top = 30;
        g.row_h = 54;
        g.row_gap = 4;
        g.env_y = 262;
        g.rate_btn_w = 38;
        g.val_font = LVGL_PORT_FONT_40;
    }
    return g;
}

typedef struct {
    lv_obj_t *row;   /**< 彩色条本体 */
    lv_obj_t *value; /**< 大数字（白字） */
    lv_obj_t *unit;  /**< 单位小字 */
    lv_obj_t *arrow; /**< 方向箭头（仅电流用） */
} metric_t;

static metric_t s_v, s_i, s_p, s_e;
static lv_obj_t *s_lbl_env;

static void create_quick_menu(lv_obj_t *tile); /* 定义见文件末尾 */

/**
 * @brief 创建一行数据条
 * @param y      该行顶部 y 坐标
 * @param row_w  行宽（随分辨率：横屏 272 / 竖屏 228）
 * @param row_h  行高（横屏 44 / 竖屏 54）
 * @param val_font 大数字字号（横屏 36 / 竖屏 40）
 */
static void create_row(lv_obj_t *parent, int y, int row_w, int row_h,
                       lvgl_port_font_size_t val_font, lv_color_t bg, metric_t *m,
                       bool with_arrow)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, row_w, row_h);
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

    /* 左：大数字（白字），Bold 填满行高（横屏 36px/44px 行，竖屏 40px/54px 行） */
    m->value = lv_label_create(row);
    lv_label_set_text(m->value, "0.00000");
    lv_obj_set_style_text_font(m->value, lvgl_port_get_font_bold(val_font), 0);
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
    lv_obj_set_style_text_font(m->unit, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    lv_obj_set_style_text_color(m->unit, lv_color_white(), 0);
}

void screen_measure_create(lv_obj_t *tile)
{
    measure_geom_t g = measure_geom();

    lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(tile, false);
    /* 不用 flex：这里全部用绝对坐标，才能精确控制与圆角的距离 */
    lv_obj_set_style_pad_all(tile, 0, 0);

    const int row_w = (int)g.w - 2 * SAFE_X;
    const int row_h = (int)g.row_h;
    const int row_pitch = row_h + (int)g.row_gap;
    create_row(tile, (int)g.row_top, row_w, row_h, g.val_font, COL_V, &s_v, false);
    create_row(tile, (int)g.row_top + row_pitch, row_w, row_h, g.val_font, COL_I, &s_i, true);
    create_row(tile, (int)g.row_top + 2 * row_pitch, row_w, row_h, g.val_font, COL_P, &s_p, false);
    create_row(tile, (int)g.row_top + 3 * row_pitch, row_w, row_h, g.val_font, COL_E, &s_e, false);

    /* 环境温湿度：底部居中（页码点已移到状态栏，底部整条留给它） */
    s_lbl_env = lv_label_create(tile);
    lv_label_set_text(s_lbl_env, "--.-℃  --%RH");
    lv_obj_set_style_text_font(s_lbl_env, lvgl_port_get_font(LVGL_PORT_FONT_16), 0);
    lv_obj_set_style_text_color(s_lbl_env, COL_ENV, 0);
    lv_obj_set_width(s_lbl_env, g.w);
    lv_obj_set_style_text_align(s_lbl_env, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_env, 0, g.env_y);

    create_quick_menu(tile);
}

/** 动态小数位格式化：保证 ≤9 字符（36px Bold 下约 194px，加单位不撞行宽）
 *  |v| < 1        → 5 位小数（0.01886）
 *  1 ≤ |v| < 100  → 4 位小数（19.9767）
 *  100 ≤ |v| <1e4 → 2 位小数（1234.56）
 *  ≥ 1e4          → 0 位小数（12345）
 *  非有限/极端值  → 安全回退（避免 inf 撑破行宽） */
static void fmt_val(char *dst, size_t n, float v)
{
    if (!isfinite(v)) {
        snprintf(dst, n, "0.0000");
        return;
    }
    float a = fabsf(v);
    if (a < 1.0f) {
        snprintf(dst, n, "%.5f", (double)v);
    } else if (a < 100.0f) {
        snprintf(dst, n, "%.4f", (double)v);
    } else if (a < 10000.0f) {
        snprintf(dst, n, "%.2f", (double)v);
    } else if (a < 1e6f) {
        snprintf(dst, n, "%.0f", (double)v);
    } else {
        snprintf(dst, n, "%.2e", (double)v);
    }
}

void screen_measure_refresh(const data_store_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }

    char buf[24];

    fmt_val(buf, sizeof(buf), snap->voltage_v);
    lv_label_set_text(s_v.value, buf);
    lv_label_set_text(s_v.unit, "V");

    fmt_val(buf, sizeof(buf), snap->current_a);
    lv_label_set_text(s_i.value, buf);
    lv_label_set_text(s_i.unit, "A");
    if (s_i.arrow != NULL) {
        /* 箭头随屏幕角度自适应：电流的物理流向固定，屏幕转到哪个角度，
         * 箭头就指向屏幕坐标系里的对应方向（0°→右/90°→下/180°→左/270°→上），
         * 负电流取反向。下标 = 角度/90。 */
        static const char *const arrow_pos[4] = { "▶", "▼", "◀", "▲" };
        static const char *const arrow_neg[4] = { "◀", "▲", "▶", "▼" };
        int t = (int)(lvgl_port_get_rotation_angle() / 90) & 3;
        lv_label_set_text(s_i.arrow, (snap->current_a < 0.0f) ? arrow_neg[t] : arrow_pos[t]);
    }

    fmt_val(buf, sizeof(buf), snap->power_w);
    lv_label_set_text(s_p.value, buf);
    lv_label_set_text(s_p.unit, "W");

    fmt_val(buf, sizeof(buf), snap->energy_wh);
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

/* ============================================================
 * 快速菜单（全程按键可操作，不依赖触屏）
 *
 * 打开：测量页 K2 长按。关闭：K1 或 K2 长按（开关式）。
 * 菜单内：
 *   · K1 单击 = 焦点上移，K2 单击 = 焦点下移（两行循环，带焦点高亮 + 反馈音）；
 *   · K2 双击 = 激活当前焦点行：
 *       -「清除电能统计数据」→ 执行清零，**自动关闭菜单**；
 *       -「数据刷新率」→ 在 1/5/10/20/30Hz 间循环切换并立即持久化。
 * 触屏同样可用：点任意档位按钮直接切换，点清除行直接清零并关闭。
 * ============================================================ */
#define MENU_H 152
#define MENU_FOCUS_CNT 2 /* 0=清除行，1=刷新率行 */
static lv_obj_t *s_menu = NULL;
static bool s_menu_open = false;
static int s_focus = 0;
static lv_obj_t *s_clear_btn = NULL;
static lv_obj_t *s_rate_row = NULL;
static lv_obj_t *s_rate_btns[5];
static const uint8_t s_rate_vals[5] = { 1, 5, 10, 20, 30 };
static const char *s_rate_texts[5] = { "1Hz", "5Hz", "10Hz", "20Hz", "30Hz" };

/** 迷你按钮（快速菜单内使用） */
static lv_obj_t *menu_btn(lv_obj_t *parent, const char *text, int32_t w, int32_t h,
                          lvgl_port_font_size_t fsize)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xE8F0FE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, lvgl_port_get_font(fsize), 0);
    lv_obj_set_style_text_color(l, COL_INK, 0);
    lv_obj_center(l);
    return b;
}

/** 焦点高亮：被聚焦的整行描橙色粗边；刷新率行内当前档位另有蓝底白字 */
static void menu_focus_refresh(void)
{
    const lv_coord_t fw = s_focus == 0 ? 2 : 1;
    lv_color_t fc = s_focus == 0 ? COL_PRIMARY : COL_BORDER;
    if (s_clear_btn != NULL) {
        lv_obj_set_style_border_width(s_clear_btn, fw, 0);
        lv_obj_set_style_border_color(s_clear_btn, fc, 0);
    }
    if (s_rate_row != NULL) {
        lv_obj_set_style_border_width(s_rate_row, s_focus == 1 ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_rate_row, COL_PRIMARY, 0);
    }
}

/** 刷新率按钮高亮（当前档） */
static void menu_rate_refresh(void)
{
    uint8_t cur = ui_get_display_rate_hz();
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = s_rate_btns[i];
        if (b == NULL) {
            continue;
        }
        bool sel = (s_rate_vals[i] == cur);
        lv_obj_set_style_bg_color(b, sel ? COL_PRIMARY : lv_color_white(), 0);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        if (l != NULL) {
            lv_obj_set_style_text_color(l, sel ? lv_color_white() : COL_INK, 0);
        }
    }
}

/** 关闭快速菜单（位置按当前分辨率沉到屏外；横屏 y=240 / 竖屏 y=284） */
static void menu_close(void)
{
    measure_geom_t g = measure_geom();
    lv_obj_set_hidden(s_menu, true);
    lv_obj_set_pos(s_menu, 0, g.h);
    s_menu_open = false;
}

/** 清零动作（触屏/按键共用）：执行 + 提示音 + 自动关闭菜单 */
static void menu_do_clear(void)
{
    (void)power_measure_reset_energy();
    (void)buzzer_play(BUZZER_TONE_OK);
    if (s_menu_open) {
        menu_close();
    }
}

/** 刷新率切到下一档（1→5→10→20→30→1 循环） */
static void menu_rate_cycle(void)
{
    static const uint8_t order[5] = { 1, 5, 10, 20, 30 };
    uint8_t cur = ui_get_display_rate_hz();
    uint8_t next = order[0];
    for (int i = 0; i < 5; i++) {
        if (order[i] == cur) {
            next = order[(i + 1) % 5];
            break;
        }
    }
    ui_set_display_rate_hz(next);
    menu_rate_refresh();
    (void)buzzer_play(BUZZER_TONE_CLICK);
}

static void menu_clear_clicked_cb(lv_event_t *e)
{
    (void)e;
    menu_do_clear();
}

static void menu_rate_clicked_cb(lv_event_t *e)
{
    uint8_t hz = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    ui_set_display_rate_hz(hz);
    menu_rate_refresh();
    (void)buzzer_play(BUZZER_TONE_CLICK);
}

static void create_quick_menu(lv_obj_t *tile)
{
    measure_geom_t g = measure_geom();

    /* 重建场景（旋转后 ui_nav 重建本页）：菜单随之新建，先复位开关态，
     * 避免 s_menu_open 残留为 true 导致按键被投递给一个隐藏的新菜单。 */
    s_menu_open = false;
    s_focus = 0;
    s_clear_btn = NULL;
    s_rate_row = NULL;
    for (int i = 0; i < 5; i++) {
        s_rate_btns[i] = NULL;
    }

    s_menu = lv_obj_create(tile);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, g.w, MENU_H);
    lv_obj_set_pos(s_menu, 0, g.h); /* 屏外（关闭态，横 240 / 竖 284） */
    lv_obj_set_style_bg_color(s_menu, COL_CARD, 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_menu, 12, 0);
    lv_obj_set_style_border_color(s_menu, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_menu, 1, 0);
    lv_obj_set_style_pad_all(s_menu, 8, 0);
    lv_obj_set_style_pad_row(s_menu, 4, 0);
    lv_obj_set_scrollable(s_menu, false);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_hidden(s_menu, true);

    /* 标题区：标题与按键提示分两行，避免窄屏（竖屏 240 宽）同行溢出重叠 */
    lv_obj_t *hdr = lv_obj_create(s_menu);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_scrollable(hdr, false);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, "快速菜单");
    lv_obj_set_style_text_font(t, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    lv_obj_set_style_text_color(t, COL_INK, 0);

    lv_obj_t *hint = lv_label_create(hdr);
    lv_label_set_text(hint, "K1/K2 上下 · 双击确定 · 长按退出");
    lv_obj_set_style_text_font(hint, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(hint, COL_ENV, 0);

    /* ① 清除电能统计（焦点行 0） */
    s_clear_btn = menu_btn(s_menu, "清除电能统计数据", LV_PCT(100), 38, LVGL_PORT_FONT_16);
    lv_obj_add_event_cb(s_clear_btn, menu_clear_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* ② 数据刷新率（焦点行 1；按钮行自身描边作为焦点指示） */
    lv_obj_t *cap = lv_label_create(s_menu);
    lv_label_set_text(cap, "数据刷新率（低档位做滑动平均，读数更稳）");
    lv_obj_set_style_text_font(cap, lvgl_port_get_font(LVGL_PORT_FONT_12), 0);
    lv_obj_set_style_text_color(cap, COL_ENV, 0);

    s_rate_row = lv_obj_create(s_menu);
    lv_obj_remove_style_all(s_rate_row);
    lv_obj_set_size(s_rate_row, LV_PCT(100), 34);
    lv_obj_set_scrollable(s_rate_row, false);
    lv_obj_set_flex_flow(s_rate_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_rate_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(s_rate_row, 8, 0);
    lv_obj_set_style_border_color(s_rate_row, COL_PRIMARY, 0);

    for (int i = 0; i < 5; i++) {
        /* 按钮宽随分辨率：横屏 48（5 个+间隙在 264 内宽内），
         * 竖屏收窄到 38（5×38=190，在 220 内宽内仍均布可点）。 */
        s_rate_btns[i] =
            menu_btn(s_rate_row, s_rate_texts[i], g.rate_btn_w, 34, LVGL_PORT_FONT_12);
        lv_obj_add_event_cb(s_rate_btns[i], menu_rate_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_rate_vals[i]);
    }
    menu_rate_refresh();
    menu_focus_refresh();
}

void screen_measure_open_menu(void)
{
    if (s_menu == NULL) {
        return;
    }
    measure_geom_t g = measure_geom();
    s_focus = 0; /* 每次打开焦点回到第一行 */
    menu_rate_refresh();
    menu_focus_refresh();
    lv_obj_set_hidden(s_menu, false);
    lv_obj_set_pos(s_menu, 0, g.h - MENU_H); /* 自底部升起：横 92 / 竖 136 */
    s_menu_open = true;
    /* 按键反馈音由 ui_nav_handle_key 统一播放，触屏路径（screen_wave 风格手势
     * 不会走到这里）无需本处再响——避免双响。 */
}

bool screen_measure_menu_is_open(void)
{
    return s_menu_open;
}

bool screen_measure_menu_key(button_id_t id, button_event_t ev)
{
    if (!s_menu_open) {
        return false;
    }

    if (ev == BUTTON_EVENT_LONG_PRESS_START) {
        /* K1/K2 长按均关闭（打开也是 K2 长按，开关式）；按键音由外层统一响 */
        menu_close();
        return true;
    }

    if (ev == BUTTON_EVENT_SINGLE_CLICK) {
        /* K1 = 焦点上移，K2 = 焦点下移（循环）；按键音由外层统一响 */
        if (id == BUTTON_ID_BOOT) {
            s_focus = (s_focus - 1 + MENU_FOCUS_CNT) % MENU_FOCUS_CNT;
        } else {
            s_focus = (s_focus + 1) % MENU_FOCUS_CNT;
        }
        menu_focus_refresh();
        return true;
    }

    if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
        /* K2 双击 = 激活当前焦点行（K1 双击同样有效，减少记忆负担） */
        if (s_focus == 0) {
            menu_do_clear(); /* 内部会自动关闭菜单 */
        } else {
            menu_rate_cycle();
        }
        return true;
    }

    return false;
}
