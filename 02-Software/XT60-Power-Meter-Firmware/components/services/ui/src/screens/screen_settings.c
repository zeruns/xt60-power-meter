/**
 * @file screen_settings.c
 * @brief 设置页：2×3 功能网格入口 + 五个功能二级页（v0.2.0 设置全可用）
 *
 * ────────────────────────────────────────────────────────────
 * 版式（284×240；顶部 22px 状态栏由 ui_nav 共享，不在本页职责内）
 * ────────────────────────────────────────────────────────────
 * 首页（网格一屏尽收，无需滚动）：
 *   ┌ x=14 ──────────────────── x=270 ┐
 *   │ 设置                              │ y≈28 标题（Bold 20）
 *   │ ┌──────────┐ ┌──────────┐        │ y=52
 *   │ │ 测量设置  │ │ 显示设置  │        │ 每格 122×48
 *   │ └──────────┘ └──────────┘        │
 *   │ ┌──────────┐ ┌──────────┐        │ y=108
 *   │ │ 蜂鸣器    │ │ WiFi     │        │
 *   │ └──────────┘ └──────────┘        │
 *   │ ┌──────────┐ ┌──────────┐        │ y=164
 *   │ │ 电能统计  │ │ 关于本机  │        │
 *   │ └──────────┘ └──────────┘        │
 *   └──────────────────────────────────┘
 *
 * 二级页：覆盖整个 tile 的浮层（随 tile 一起被横向滑动，不跨页），
 * 顶部 36px 标题栏（← 返回 + 标题），下方内容区自行滚动。
 *
 * ────────────────────────────────────────────────────────────
 * 与服务的边界
 * ────────────────────────────────────────────────────────────
 *   · 响度：settings（持久化）+ buzzer（立即生效）；
 *   · 亮度：settings（持久化）+ lcd_nv3030b（立即生效）；
 *   · 标定：power_measure_get/set_calibration（单点增益校准，
 *     修正"读数 × 增益 = 标准值"的增益系数；零点 offset 保持原值）；
 *   · 电能清零：power_measure_reset_energy（测量核在块边界执行，防跨核撕裂）；
 *   · WiFi：v0.5.0 才提供联网能力，本页为路线图说明（诚实预期管理）。
 *
 * ────────────────────────────────────────────────────────────
 * 实时数值刷新
 * ────────────────────────────────────────────────────────────
 * 校准向导与电能统计页需要实时值，由 ui_nav_refresh 以 5Hz 转发快照到
 * screen_settings_refresh()，本文件按"当前二级页类型"分发更新对应标签。
 */
#include "screen_settings.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "lvgl.h"
#include "lvgl_port.h"

#include "buzzer.h"
#include "lcd_nv3030b.h"
#include "orientation.h"
#include "power_measure.h"
#include "settings.h"

/* ---- 色板（与测量页/波形页同一语义色） ---- */
#define COL_PRIMARY  lv_color_hex(0x0066CC)
#define COL_INK      lv_color_hex(0x1A1A1A)
#define COL_GRAY     lv_color_hex(0x6B7280)
#define COL_BORDER   lv_color_hex(0xB4BEC9) /* 控件边框：边界清晰（原 #E3E7EC 过浅） */
#define COL_CARD_BG  lv_color_hex(0xF1F4F8) /* 卡片底：与白底拉开可辨差异 */
#define COL_PRESSED  lv_color_hex(0xE8F0FE)
#define COL_OK       lv_color_hex(0x1E9E4F)
#define COL_DANGER   lv_color_hex(0xD0392B)

/* ---- 首页网格几何 ---- */
#define GRID_SIDE_PAD 14 /**< 左右内缩（避开面板圆角） */
#define GRID_TOP_Y    52 /**< 网格起始 y */
#define CELL_HEIGHT   48 /**< 单格高度（≥40px 触摸目标标准） */
#define CELL_GAP_X    10
#define CELL_GAP_Y    8

/* ---- 二级页 ---- */
#define SUB_TOPBAR_H 36

/** 滑条范围（与 settings/buzzer 驱动的合法范围一致） */
#define BRIGHT_MIN 10
#define BRIGHT_MAX 100
#define VOLUME_MIN 1
#define VOLUME_MAX 100

/* ============================================================
 * 模块状态
 * ============================================================ */

/** 当前打开的二级页类型（SUB_NONE = 在首页） */
typedef enum {
    SUB_NONE = 0,
    SUB_CALIB_MENU, /**< 测量设置：通道选择 */
    SUB_CALIB_EDIT, /**< 标定向导：标准值输入 */
    SUB_CALIB_DONE, /**< 标定完成提示（1.4s 后自动返回） */
    SUB_DISPLAY,    /**< 显示设置：亮度 + 方向 */
    SUB_BUZZER,     /**< 蜂鸣器响度 */
    SUB_WIFI,       /**< WiFi 路线图说明 */
    SUB_ENERGY,     /**< 电能统计与清零 */
} sub_kind_t;

static lv_obj_t *s_tile = NULL;      /**< 设置页 tile（二级页浮层的父级） */
static lv_obj_t *s_subpage = NULL;   /**< 二级页浮层；NULL = 未打开 */
static lv_obj_t *s_sub_content = NULL; /**< 二级页内容容器 */
static sub_kind_t s_sub_kind = SUB_NONE;
static bool s_layer_hooked = false;  /**< 对话框样式与关闭处理是否已安装 */
static lv_obj_t *s_msgbox_scrim = NULL; /**< 当前对话框的压暗遮罩（随框删除） */

static data_store_snapshot_t s_last_snap; /**< 最近快照（校准向导/电能统计使用） */

/* 刷新用标签（所在二级页关闭时置 NULL） */
static lv_obj_t *s_cal_read_label = NULL;     /**< 校准向导：当前读数 */
static lv_obj_t *s_energy_wh_label = NULL;    /**< 电能统计：累计电能 */
static lv_obj_t *s_energy_rt_label = NULL;    /**< 电能统计：运行时间 */
static lv_obj_t *s_bright_value_label = NULL; /**< 显示设置：亮度数值 */
static lv_obj_t *s_vol_value_label = NULL;    /**< 蜂鸣器：响度数值 */

/* 校准向导状态 */
static bool s_cal_is_voltage = true; /**< true=电压通道，false=电流通道 */
static float s_cal_target = 0.0f;    /**< 标准值（用户输入） */
static int s_cal_step_idx = 1;       /**< 步长档位索引（见 s_cal_steps） */
static lv_obj_t *s_cal_target_label = NULL;

/** 校准步长档位：粗调 → 细调循环切换 */
static const float s_cal_steps[] = { 1.0f, 0.1f, 0.01f, 0.001f };
#define CAL_STEP_COUNT ((int)(sizeof(s_cal_steps) / sizeof(s_cal_steps[0])))

/** 方向按钮（角度按钮高亮当前实际方向） */
static lv_obj_t *s_angle_btns[4] = { 0 }; /**< 0/90/180/270 度按钮 */
static lv_obj_t *s_auto_switch = NULL;    /**< 重力自动旋转开关 */
static const uint16_t s_angle_vals[4] = { 0, 90, 180, 270 };

/* ============================================================
 * 对话框（lv_msgbox + 顶层图层可点击技巧，沿用原实现）
 * ============================================================ */

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
 * @brief 延迟一帧复查：图层已无子对象则恢复"不可点击"
 *
 * 时序说明：LV_EVENT_DELETE 触发时，被删对象**可能仍留在父对象的子列表中**，
 * 此刻直接查 child_count 会拿到非 0 值，导致顶层图层永远保持 clickable、
 * 把后续所有触摸全部吞掉（表现为"对话框关掉后返回按钮点不动"）。
 * 因此把复查放到下一帧（lv_async_call）执行。
 */
static void layer_restore_check_cb(void *layer_ptr)
{
    lv_obj_t *layer = (lv_obj_t *)layer_ptr;
    if (layer != NULL && lv_obj_get_child_count(layer) == 0) {
        lv_obj_set_clickable(layer, false);
    }
}

/**
 * @brief 对话框被删除 → 删除压暗遮罩 + 安排一次延迟复查（恢复图层不可点击）
 */
static void box_deleted_cb(lv_event_t *e)
{
    lv_obj_t *layer = (lv_obj_t *)lv_event_get_user_data(e);
    if (s_msgbox_scrim != NULL) {
        lv_obj_delete(s_msgbox_scrim);
        s_msgbox_scrim = NULL;
    }
    if (layer != NULL) {
        lv_async_call(layer_restore_check_cb, layer);
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
            lv_obj_set_style_text_color(lv_obj_get_child(content, i), lv_color_hex(0x374151),
                                        0);
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

/** @brief 创建挂到顶层图层上的模态框（公共部分） */
static lv_obj_t *msgbox_begin(const char *title, const char *text)
{
    lv_obj_t *layer = lv_layer_top();

    /* 关键：让顶层图层参与命中测试，否则里面的按钮点不动（见原实现注释） */
    lv_obj_set_clickable(layer, true);
    if (!s_layer_hooked) {
        s_layer_hooked = true;
        lv_obj_add_event_cb(layer, layer_clicked_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 压暗遮罩：先于对话框创建（渲染在下层），模态感 + 突出对话框。
     * 不可点击 → 命中测试穿透到顶层图层本体，点遮罩空白处仍走
     * layer_clicked_cb 关闭对话框（与原交互一致）。 */
    lv_obj_t *scrim = lv_obj_create(layer);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_40, 0);
    lv_obj_set_clickable(scrim, false);
    s_msgbox_scrim = scrim;

    lv_obj_t *box = lv_msgbox_create(layer);
    lv_obj_set_width(box, 250); /* 固定宽度，避免长文本撑到圆角外 */
    lv_msgbox_add_title(box, title);
    lv_msgbox_add_text(box, text);
    style_msgbox(box);
    lv_obj_center(box);

    /* 对话框删除后恢复图层可点击性 */
    lv_obj_add_event_cb(box, box_deleted_cb, LV_EVENT_DELETE, layer);
    return box;
}

static void notice_ok_cb(lv_event_t *e); /* 前向声明（定义见下方） */

/** @brief 单按钮提示框（"确定"，点击显式关闭） */
static void show_notice(const char *title, const char *text)
{
    lv_obj_t *box = msgbox_begin(title, text);
    lv_obj_t *ok = lv_msgbox_add_footer_button(box, "确定");
    lv_obj_add_event_cb(ok, notice_ok_cb, LV_EVENT_CLICKED, NULL);
    style_msgbox(box);
}

/** 确认框的"确定"动作回调 */
typedef void (*confirm_cb_t)(void);

/**
 * @brief 从 footer 按钮向上找到所属 msgbox 并**显式关闭**（幂等）
 *
 * 防御式设计：对话框的关闭不依赖 LVGL footer 按钮的内建关闭行为——
 * 若该行为在某种情况下失效，对话框将一直留在顶层图层上，把后续所有触摸
 * 全部吞掉（表现为"对话框关不掉、返回按钮也点不动"）。显式关闭双保险。
 */
static void msgbox_close_from_btn(lv_obj_t *btn)
{
    lv_obj_t *o = btn;
    while (o != NULL && !lv_obj_check_type(o, &lv_msgbox_class)) {
        o = lv_obj_get_parent(o);
    }
    if (o != NULL) {
        lv_msgbox_close(o);
    }
}

/** 单按钮提示框："确定"点击 → 显式关闭 */
static void notice_ok_cb(lv_event_t *e)
{
    msgbox_close_from_btn(lv_event_get_target(e));
}

static void confirm_ok_cb(lv_event_t *e)
{
    confirm_cb_t cb = (confirm_cb_t)lv_event_get_user_data(e);
    /* 先关闭本框（同步删除，立即生效），再执行业务动作（动作可能弹新框） */
    msgbox_close_from_btn(lv_event_get_target(e));
    if (cb != NULL) {
        cb();
    }
}

/** @brief 确认框（取消 / 确定） */
static void show_confirm(const char *title, const char *text, confirm_cb_t ok_cb)
{
    lv_obj_t *box = msgbox_begin(title, text);
    lv_msgbox_add_footer_button(box, "取消");
    lv_obj_t *ok = lv_msgbox_add_footer_button(box, "确定");
    lv_obj_add_event_cb(ok, confirm_ok_cb, LV_EVENT_CLICKED, (void *)ok_cb);
    style_msgbox(box);
}

/* ============================================================
 * 通用小部件
 * ============================================================ */

/** @brief 透明容器（去样式，不改 flex 行为） */
static lv_obj_t *plain_container(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    return o;
}

/** @brief 文本标签（指定字号/颜色/字重） */
static lv_obj_t *plain_label(lv_obj_t *parent, const char *text, lvgl_port_font_size_t size,
                             lv_color_t color, bool bold)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, bold ? lvgl_port_get_font_bold(size)
                                       : lvgl_port_get_font(size),
                               0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

/** @brief 通用主按钮（白底描边，按下态浅蓝） */
static lv_obj_t *plain_button(lv_obj_t *parent, const char *text, int32_t w, int32_t h)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_color(btn, COL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, COL_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *lbl = plain_label(btn, text, LVGL_PORT_FONT_16, COL_INK, false);
    lv_obj_center(lbl);
    return btn;
}

/* ============================================================
 * 二级页框架
 * ============================================================ */

/** @brief 关闭当前二级页（幂等） */
static void subpage_close(void)
{
    if (s_subpage != NULL) {
        lv_obj_delete(s_subpage);
    }
    s_subpage = NULL;
    s_sub_content = NULL;
    s_sub_kind = SUB_NONE;
    s_cal_read_label = NULL;
    s_cal_target_label = NULL;
    s_energy_wh_label = NULL;
    s_energy_rt_label = NULL;
    s_bright_value_label = NULL;
    s_vol_value_label = NULL;
    memset(s_angle_btns, 0, sizeof(s_angle_btns));
    s_auto_switch = NULL;
}

/** @brief 二级页"返回"按钮 */
static void back_clicked_cb(lv_event_t *e)
{
    (void)e;
    subpage_close();
}

/**
 * @brief 打开二级页浮层并创建标题栏，返回内容容器
 *
 * 浮层覆盖整个设置 tile：滚动方向设为 ALL（即使内容不溢出也吞掉滑动），
 * 避免在二级页上左滑时把 tileview 翻到别的页。
 */
static lv_obj_t *subpage_open(sub_kind_t kind, const char *title)
{
    subpage_close();
    s_sub_kind = kind;

    s_subpage = lv_obj_create(s_tile);
    lv_obj_remove_style_all(s_subpage);
    lv_obj_set_size(s_subpage, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_subpage, 0, 0);
    lv_obj_set_style_bg_color(s_subpage, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_subpage, LV_OPA_COVER, 0);
    lv_obj_set_scroll_dir(s_subpage, LV_DIR_ALL); /* 吞掉滑动，防误翻页 */
    lv_obj_set_scrollbar_mode(s_subpage, LV_SCROLLBAR_MODE_OFF);
    /* 垂直 flex 排布：标题栏在上、内容区占满剩余高度。两块区域**不重叠**，
     * 内容容器永远不会盖住"返回"按钮（曾因全屏覆盖导致返回键点不到）。
     * 顶部 22px 内缩让开共享状态栏。 */
    lv_obj_set_style_pad_top(s_subpage, 22, 0);
    lv_obj_set_flex_flow(s_subpage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_subpage, 0, 0);

    /* ---- 标题栏 ---- */
    lv_obj_t *bar = lv_obj_create(s_subpage);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), SUB_TOPBAR_H);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_size(back, 72, 28);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(back, lv_color_white(), 0);
    lv_obj_set_style_bg_color(back, COL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_border_color(back, COL_BORDER, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_add_event_cb(back, back_clicked_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *bl = plain_label(back, "< 返回", LVGL_PORT_FONT_14, COL_INK, false);
        lv_obj_center(bl);
    }

    lv_obj_t *tl = plain_label(bar, title, LVGL_PORT_FONT_16, COL_INK, true);
    lv_obj_align(tl, LV_ALIGN_CENTER, 0, 0);

    /* ---- 内容容器（flex 剩余高度，从标题栏下方开始，不与其重叠） ---- */
    lv_obj_t *c = lv_obj_create(s_subpage);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, 0);
    lv_obj_set_flex_grow(c, 1); /* 占满标题栏以下的剩余高度 */
    lv_obj_set_style_pad_top(c, 8, 0); /* 与标题栏的间距 */
    lv_obj_set_style_pad_hor(c, 14, 0);
    lv_obj_set_style_pad_bottom(c, 10, 0);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_AUTO);

    s_sub_content = c;
    return c;
}

/* ============================================================
 * 二级页：测量设置（标定向导）
 * ============================================================ */

static void calib_ui_enter(void); /* 前向声明 */

/** 进入校准编辑界面（标准值输入） */
static void calib_channel_clicked_cb(lv_event_t *e)
{
    s_cal_is_voltage = (bool)(intptr_t)lv_event_get_user_data(e);
    calib_ui_enter();
}

/** @brief 通道选择页（测量设置的首页） */
static void calib_ui_menu(void)
{
    lv_obj_t *c = subpage_open(SUB_CALIB_MENU, "测量设置");

    lv_obj_t *hint = plain_label(c,
                                 "用标准表对照修正读数（单点增益校准）：\n"
                                 "挂上已知负载，输入标准表读数后按“应用”。",
                                 LVGL_PORT_FONT_12, COL_GRAY, false);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    /* 当前标定摘要 */
    power_measure_calib_t cal;
    power_measure_get_calibration(&cal);
    char buf[96];
    lv_obj_t *card = plain_container(c);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_style_bg_color(card, COL_CARD_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);

    snprintf(buf, sizeof(buf), "电压增益 %.6e V/LSB", (double)cal.voltage_gain);
    plain_label(card, buf, LVGL_PORT_FONT_12, COL_GRAY, false);
    snprintf(buf, sizeof(buf), "电流增益 %.6e A/LSB", (double)cal.current_gain);
    plain_label(card, buf, LVGL_PORT_FONT_12, COL_GRAY, false);

    /* 通道按钮：并排一行（各 50%）。竖排两行时横屏下内容高约 206px >
     * 可用 182px，第二个按钮会被底部裁掉一半，需滚动才能点全；
     * 并排后总高降到 162px，两个按钮都完整可点。 */
    lv_obj_t *row = plain_container(c);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t *bv = plain_button(row, "电压校准", LV_PCT(50), 44);
    lv_obj_add_event_cb(bv, calib_channel_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *bi = plain_button(row, "电流校准", LV_PCT(50), 44);
    lv_obj_add_event_cb(bi, calib_channel_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
}

/** 校准页数值/步长变化时刷新目标值显示 */
static void calib_target_update_label(void)
{
    if (s_cal_target_label == NULL) {
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%.4f %s", (double)s_cal_target,
             s_cal_is_voltage ? "V" : "A");
    lv_label_set_text(s_cal_target_label, buf);
}

static void calib_step_cycle_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    s_cal_step_idx = (s_cal_step_idx + 1) % CAL_STEP_COUNT;
    char buf[32];
    snprintf(buf, sizeof(buf), "步长 %.3f", (double)s_cal_steps[s_cal_step_idx]);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl != NULL) {
        lv_label_set_text(lbl, buf);
    }
}

static void calib_minus_cb(lv_event_t *e)
{
    (void)e;
    s_cal_target -= s_cal_steps[s_cal_step_idx];
    if (s_cal_target < 0.0f) {
        s_cal_target = 0.0f;
    }
    calib_target_update_label();
}

static void calib_plus_cb(lv_event_t *e)
{
    (void)e;
    s_cal_target += s_cal_steps[s_cal_step_idx];
    calib_target_update_label();
}

/** 关闭二级页的一次性定时器（校准完成提示后自动返回） */
static void close_timer_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    subpage_close();
}

/** @brief 应用校准：new_gain = old_gain × (标准值 / 当前读数) */
static void calib_apply_cb(lv_event_t *e)
{
    (void)e;
    float reading = s_cal_is_voltage ? s_last_snap.voltage_v : s_last_snap.current_a;
    float min_reading = s_cal_is_voltage ? 0.05f : 0.001f; /* 读数过小视为未加负载 */
    if (fabsf(reading) < min_reading) {
        show_notice("无法校准", "当前读数过小，\n请先接入负载/标准源再试。");
        buzzer_play(BUZZER_TONE_ERROR);
        return;
    }
    float ratio = s_cal_target / reading;
    if (ratio < 0.5f || ratio > 2.0f) {
        show_notice("偏差过大", "标准值与读数相差超过 2 倍，\n请检查输入是否正确。");
        buzzer_play(BUZZER_TONE_ERROR);
        return;
    }

    power_measure_calib_t cal;
    power_measure_get_calibration(&cal);
    if (s_cal_is_voltage) {
        cal.voltage_gain *= ratio;
    } else {
        cal.current_gain *= ratio;
    }
    if (power_measure_set_calibration(&cal) != ESP_OK) {
        show_notice("校准失败", "参数保存失败，请重试。");
        buzzer_play(BUZZER_TONE_ERROR);
        return;
    }

    buzzer_play(BUZZER_TONE_OK);

    /* 结果提示（同页居中显示，1.4s 后自动返回首页） */
    lv_obj_t *c = subpage_open(SUB_CALIB_DONE, "测量设置");
    char buf[64];
    snprintf(buf, sizeof(buf), "校准完成\n增益已修正 ×%.4f", (double)ratio);
    lv_obj_t *done = plain_label(c, buf, LVGL_PORT_FONT_16, COL_OK, true);
    lv_obj_set_style_text_align(done, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(done, 24, 0);
    lv_timer_create(close_timer_cb, 1400, NULL);
}

/**
 * @brief 电流零点一键清零（空载校零）
 *
 * 输出空载时点击：把电流零偏 offset 调到「抵消当前读数」的位置，
 * 使显示立即归零。物理量 = raw×gain + offset，故
 *   offset_new = offset_old − 当前读数
 * 之后再施加负载，读数即从 0 起算（消除前端零点漂移）。
 */
static void calib_zero_cb(lv_event_t *e)
{
    (void)e;
    power_measure_calib_t cal;
    power_measure_get_calibration(&cal);
    cal.current_offset -= s_last_snap.current_a;
    if (power_measure_set_calibration(&cal) != ESP_OK) {
        show_notice("清零失败", "参数保存失败，请重试。");
        return;
    }
    show_notice("已清零", "电流零点已校准。\n（空载读数归零）");
}

/** @brief 校准编辑界面（标准值输入） */
static void calib_ui_enter(void)
{
    lv_obj_t *c = subpage_open(SUB_CALIB_EDIT, s_cal_is_voltage ? "校准 · 电压" : "校准 · 电流");

    /* 目标值以当前读数为初始值（用户在此基础上按标准表修正） */
    float reading = s_cal_is_voltage ? s_last_snap.voltage_v : s_last_snap.current_a;
    s_cal_target = fabsf(reading);
    s_cal_step_idx = s_cal_is_voltage ? 2 : 3; /* 电压 0.01，电流 0.001 */

    /* 紧凑排布：确保"应用校准"按钮首屏可见（无须滚动） */
    lv_obj_set_style_pad_row(c, 6, 0);

    /* 当前读数（实时刷新） */
    s_cal_read_label = plain_label(c, "--", LVGL_PORT_FONT_24, COL_PRIMARY, true);
    char buf[48];
    snprintf(buf, sizeof(buf), "%.5f %s", (double)reading, s_cal_is_voltage ? "V" : "A");
    lv_label_set_text(s_cal_read_label, buf);

    /* 标准值编辑 */
    lv_obj_t *row = plain_container(c);
    lv_obj_set_size(row, LV_PCT(100), 36);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);

    /* 注意：用全角"－/＋"（GB2312 子集内），不要用 U+2212 数学减号——
     * 字体子集不含该字形，真机会显示为方框（模拟器已复现）。 */
    lv_obj_t *minus = plain_button(row, "－", 56, 36);
    lv_obj_add_event_cb(minus, calib_minus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *minus_lbl = lv_obj_get_child(minus, 0);
    if (minus_lbl != NULL) {
        lv_obj_set_style_text_font(minus_lbl, lvgl_port_get_font_bold(LVGL_PORT_FONT_20), 0);
    }

    s_cal_target_label = plain_label(row, "0.0000", LVGL_PORT_FONT_20, COL_INK, true);
    lv_obj_set_width(s_cal_target_label, 120);
    lv_obj_set_style_text_align(s_cal_target_label, LV_TEXT_ALIGN_CENTER, 0);
    calib_target_update_label();

    lv_obj_t *plus = plain_button(row, "＋", 56, 36);
    lv_obj_add_event_cb(plus, calib_plus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus_lbl = lv_obj_get_child(plus, 0);
    if (plus_lbl != NULL) {
        lv_obj_set_style_text_font(plus_lbl, lvgl_port_get_font_bold(LVGL_PORT_FONT_20), 0);
    }

    /* 步长切换 */
    char step_buf[32];
    snprintf(step_buf, sizeof(step_buf), "步长 %.3f", (double)s_cal_steps[s_cal_step_idx]);
    lv_obj_t *step = plain_button(c, step_buf, LV_PCT(60), 30);
    lv_obj_add_event_cb(step, calib_step_cycle_cb, LV_EVENT_CLICKED, NULL);

    /* 电流通道：一键清零（空载校零，输出空载时抵消当前读数） */
    if (!s_cal_is_voltage) {
        lv_obj_t *zero = plain_button(c, "一键清零（空载）", LV_PCT(100), 36);
        lv_obj_add_event_cb(zero, calib_zero_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 应用 */
    lv_obj_t *apply = plain_button(c, "应用校准", LV_PCT(100), 40);
    lv_obj_set_style_bg_color(apply, COL_PRIMARY, 0);
    lv_obj_set_style_border_color(apply, COL_PRIMARY, 0);
    lv_obj_t *apply_lbl = lv_obj_get_child(apply, 0);
    if (apply_lbl != NULL) {
        lv_obj_set_style_text_color(apply_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(apply_lbl, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    }
    lv_obj_add_event_cb(apply, calib_apply_cb, LV_EVENT_CLICKED, NULL);
}

/* ============================================================
 * 二级页：显示设置（亮度 + 方向）
 * ============================================================ */

/** 更新角度按钮高亮：永远反映当前**实际显示角度**（自动模式下跟随 IMU） */
static void angle_buttons_refresh(void)
{
    uint16_t cur = lvgl_port_get_rotation_angle();
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = s_angle_btns[i];
        if (btn == NULL) {
            continue;
        }
        bool sel = (s_angle_vals[i] == cur);
        lv_obj_set_style_bg_color(btn, sel ? COL_PRIMARY : lv_color_white(), 0);
        lv_obj_set_style_border_color(btn, sel ? COL_PRIMARY : COL_BORDER, 0);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl != NULL) {
            lv_obj_set_style_text_color(lbl, sel ? lv_color_white() : COL_INK, 0);
        }
    }
}

/** 手动点角度：关自动、持久化、应用该角度 */
static void angle_clicked_cb(lv_event_t *e)
{
    uint16_t deg = (uint16_t)(intptr_t)lv_event_get_user_data(e);
    if (lvgl_port_set_rotation_angle(deg) != ESP_OK) {
        return;
    }
    orientation_set_auto(false);
    (void)settings_set_orientation(0, deg);
    (void)settings_save_now();
    if (s_auto_switch != NULL && lv_obj_has_state(s_auto_switch, LV_STATE_CHECKED)) {
        lv_obj_remove_state(s_auto_switch, LV_STATE_CHECKED);
    }
    buzzer_play(BUZZER_TONE_CLICK);
    angle_buttons_refresh();
}

/** 自动旋转开关：开 → 重力接管；关 → 保持当前角度并存为手动值 */
static void auto_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    orientation_set_auto(on);
    if (on) {
        (void)settings_set_orientation(1, lvgl_port_get_rotation_angle());
    } else {
        (void)settings_set_orientation(0, lvgl_port_get_rotation_angle());
    }
    (void)settings_save_now();
    (void)buzzer_play(BUZZER_TONE_CLICK);
}

static void bright_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t val = (uint8_t)lv_slider_get_value(slider);

    /* 立即生效 + 持久化（防抖由 settings 内部处理） */
    (void)lcd_nv3030b_set_backlight(val);
    (void)settings_set_lcd_brightness(val);

    if (s_bright_value_label != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)val);
        lv_label_set_text(s_bright_value_label, buf);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        /* 用户松手即落盘，避免马上关机导致设置丢失 */
        (void)settings_save_now();
    }
}

static void display_open_cb(void)
{
    lv_obj_t *c = subpage_open(SUB_DISPLAY, "显示设置");

    /* ---- 亮度 ---- */
    settings_t cfg;
    uint8_t bright = 80;
    if (settings_get(&cfg) == ESP_OK) {
        bright = cfg.lcd_brightness;
    }

    lv_obj_t *cap = plain_container(c);
    lv_obj_set_size(cap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    plain_label(cap, "屏幕亮度", LVGL_PORT_FONT_16, COL_INK, false);
    char buf[16];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)bright);
    s_bright_value_label = plain_label(cap, buf, LVGL_PORT_FONT_16, COL_PRIMARY, true);

    lv_obj_t *slider = lv_slider_create(c);
    /* 内缩到 84%：旋钮在两端时仍离面板边缘约 30px，拇指不会按不到；
       也避开圆角侵入区（两端各多留 8px 内边距）。 */
    lv_obj_set_width(slider, LV_PCT(84));
    lv_slider_set_range(slider, BRIGHT_MIN, BRIGHT_MAX);
    lv_slider_set_value(slider, bright, LV_ANIM_OFF);
    lv_obj_set_height(slider, 24); /* 加大主体高度，触摸更好点中 */
    lv_obj_set_style_width(slider, 20, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 26, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, bright_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, bright_slider_cb, LV_EVENT_RELEASED, NULL);

    /* ---- 屏幕方向：自动开关 + 角度按钮（0°=默认横屏，顺时针）---- */
    lv_obj_t *auto_row = plain_container(c);
    lv_obj_set_size(auto_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(auto_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(auto_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(auto_row, 8, 0);
    plain_label(auto_row, "自动旋转（随握持方向）", LVGL_PORT_FONT_16, COL_INK, false);
    s_auto_switch = lv_switch_create(auto_row);
    /* 开关样式自定义：关闭态轨道 #B4BEC9、开启轨道品牌蓝。
     * ⚠ 旋钮必须加边框：纯白旋钮在白底卡片上会与背景融为一体（实拍
     * 关闭态时只见右半灰色轨道、左半旋钮"消失"）。加 1px 同色边框
     * 后，旋钮轮廓在任何背景上都清晰可辨。 */
    lv_obj_set_size(s_auto_switch, 46, 24);
    lv_obj_set_style_bg_color(s_auto_switch, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(s_auto_switch, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_auto_switch, COL_PRIMARY, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(s_auto_switch, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_auto_switch, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_auto_switch, LV_OPA_COVER, LV_PART_KNOB);
    /* 旋钮边框用深灰（#6B7280）而非轨道同色：#B4BEC9 在白底上仍偏浅，
     * 深灰 2px 轮廓让白色旋钮在任何背景上都一眼可辨。 */
    lv_obj_set_style_border_color(s_auto_switch, COL_GRAY, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_auto_switch, 1, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_auto_switch, 3, LV_PART_KNOB);
    lv_obj_set_style_radius(s_auto_switch, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    if (settings_get(&cfg) == ESP_OK && cfg.orientation_auto) {
        lv_obj_add_state(s_auto_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_auto_switch, auto_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *cap2 = plain_label(c, "屏幕方向", LVGL_PORT_FONT_16, COL_INK, false);
    lv_obj_set_width(cap2, LV_PCT(100));
    lv_obj_set_style_margin_top(cap2, 6, 0);

    lv_obj_t *rot_row = plain_container(c);
    lv_obj_set_size(rot_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rot_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    static const char *angle_names[4] = { "0°", "90°", "180°", "270°" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = plain_button(rot_row, angle_names[i], 58, 36);
        lv_obj_add_event_cb(b, angle_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_angle_vals[i]);
        s_angle_btns[i] = b;
    }
    angle_buttons_refresh();

    lv_obj_t *note = plain_label(c, "0° 为默认横屏，顺时针旋转；自动模式下角度高亮跟随设备姿态。",
                                 LVGL_PORT_FONT_12, COL_GRAY, false);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

/* ============================================================
 * 二级页：蜂鸣器响度
 * ============================================================ */

static void volume_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t val = (uint8_t)lv_slider_get_value(slider);

    (void)buzzer_set_volume(val);
    (void)settings_set_buzzer_volume(val);

    if (s_vol_value_label != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)val);
        lv_label_set_text(s_vol_value_label, buf);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        (void)settings_save_now();
    }
}

static void buzzer_test_cb(lv_event_t *e)
{
    (void)e;
    (void)buzzer_play(BUZZER_TONE_OK);
}

static void buzzer_open_cb(void)
{
    lv_obj_t *c = subpage_open(SUB_BUZZER, "蜂鸣器响度");

    uint8_t vol = 40;
    settings_t cfg;
    if (settings_get(&cfg) == ESP_OK) {
        vol = cfg.buzzer_volume;
    }
    uint8_t drv_vol = 0;
    if (buzzer_get_volume(&drv_vol) == ESP_OK) {
        vol = drv_vol;
    }

    lv_obj_t *cap = plain_container(c);
    lv_obj_set_size(cap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    plain_label(cap, "响度", LVGL_PORT_FONT_16, COL_INK, false);
    char buf[16];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)vol);
    s_vol_value_label = plain_label(cap, buf, LVGL_PORT_FONT_16, COL_PRIMARY, true);

    lv_obj_t *slider = lv_slider_create(c);
    lv_obj_set_width(slider, LV_PCT(84)); /* 与亮度滑条一致内缩，两端旋钮可触摸 */
    lv_slider_set_range(slider, VOLUME_MIN, VOLUME_MAX);
    lv_slider_set_value(slider, vol, LV_ANIM_OFF);
    lv_obj_set_height(slider, 24); /* 加大主体高度，触摸更好点中 */
    lv_obj_set_style_width(slider, 20, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 26, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, volume_slider_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *test = plain_button(c, "试听", LV_PCT(60), 40);
    lv_obj_add_event_cb(test, buzzer_test_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *note = plain_label(c, "提示音用于按键反馈与启动完成提示；\n调到 0 以上才有声音。",
                                 LVGL_PORT_FONT_12, COL_GRAY, false);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

/* ============================================================
 * 二级页：WiFi（路线图说明，联网能力在 v0.5.0）
 * ============================================================ */

static void wifi_open_cb(void)
{
    lv_obj_t *c = subpage_open(SUB_WIFI, "WiFi 与热点");

    /* 图标 24px：32px 时整页超出可视区约 8px，底部提示行被截断（实拍 f05） */
    lv_obj_t *icon = plain_label(c, LV_SYMBOL_WIFI, LVGL_PORT_FONT_24, COL_PRIMARY, false);
    (void)icon;

    lv_obj_t *main_txt = plain_label(c, "联网功能计划于 v0.5.0 提供", LVGL_PORT_FONT_16, COL_INK,
                                     true);
    lv_obj_set_style_text_align(main_txt, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *list = plain_label(c,
                                 "届时将包含：\n"
                                 "· 自托管 Web 控制台\n"
                                 "· 历史数据记录与 CSV 导出\n"
                                 "· OTA 固件在线升级",
                                 LVGL_PORT_FONT_14, COL_INK, false);
    lv_obj_set_width(list, LV_PCT(100));
    lv_label_set_long_mode(list, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_left(list, 6, 0);

    /* 短句一屏放下（原长句折行把底部提示挤出可视区，实拍 f05 底部截断） */
    lv_obj_t *note = plain_label(c, "当前版本请通过屏幕本体查看数据。", LVGL_PORT_FONT_12,
                                 COL_GRAY, false);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

/* ============================================================
 * 二级页：电能统计与清零
 * ============================================================ */

static void energy_reset_ok(void)
{
    (void)power_measure_reset_energy();
    buzzer_play(BUZZER_TONE_OK);
    show_notice("已清零", "电能与运行时间已重新开始统计。");
}

static void energy_reset_clicked_cb(lv_event_t *e)
{
    (void)e;
    show_confirm("清零统计？", "将把“累计电能”与“运行时间”归零，\n此操作不可撤销。", energy_reset_ok);
}

static void energy_open_cb(void)
{
    lv_obj_t *c = subpage_open(SUB_ENERGY, "电能统计");

    lv_obj_t *cap = plain_label(c, "累计电能（含掉电恢复）", LVGL_PORT_FONT_12, COL_GRAY, false);
    (void)cap;
    s_energy_wh_label = plain_label(c, "0.00000 Wh", LVGL_PORT_FONT_32, COL_PRIMARY, true);

    char buf[48];
    snprintf(buf, sizeof(buf), "运行时间 %u:%02u:%02u", (unsigned)(s_last_snap.runtime_s / 3600u),
             (unsigned)((s_last_snap.runtime_s % 3600u) / 60u),
             (unsigned)(s_last_snap.runtime_s % 60u));
    s_energy_rt_label = plain_label(c, buf, LVGL_PORT_FONT_14, COL_INK, false);

    snprintf(buf, sizeof(buf), "%.5f Wh", (double)s_last_snap.energy_wh);
    lv_label_set_text(s_energy_wh_label, buf);

    lv_obj_t *btn = plain_button(c, "清零统计", LV_PCT(100), 40);
    lv_obj_set_style_border_color(btn, COL_DANGER, 0);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl != NULL) {
        lv_obj_set_style_text_color(lbl, COL_DANGER, 0);
        lv_obj_set_style_text_font(lbl, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    }
    lv_obj_add_event_cb(btn, energy_reset_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* 单行短提示（原长句会折行把本页撑到需滚动；按钮降到 40px + 短句
     * 后整页一屏放下，底部不贴边）。 */
    lv_obj_t *note = plain_label(c, "清零约 0.2 秒后生效", LVGL_PORT_FONT_12,
                                 COL_GRAY, false);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
}

/* ============================================================
 * 二级页：关于本机（对话框）
 * ============================================================ */

static void about_open_cb(void)
{
    char text[112];
    snprintf(text, sizeof(text), "XT60 功率计\n固件版本 v%s\n作者博客：blog.zeruns.com",
             esp_app_get_description()->version);
    show_notice("关于本机", text);
}

/* ============================================================
 * 首页网格
 * ============================================================ */

typedef void (*cell_cb_t)(void);

/** 网格点击的中间回调（LVGL 回调签名适配） */
static void cell_clicked_cb(lv_event_t *e)
{
    cell_cb_t cb = (cell_cb_t)lv_event_get_user_data(e);
    if (cb != NULL) {
        cb();
    }
}

/** @brief 创建一个网格单元（图标 + 名称，点击进入 cb） */
static lv_obj_t *make_cell_dispatch(lv_obj_t *row, const char *icon, const char *name,
                                    cell_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_height(btn, CELL_HEIGHT);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_color(btn, COL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, COL_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 8, 0);

    /* 图标与文字同字号（16px），但图标符号来自 montserrat14 fallback，
     * 字形 box_h/ofs_y 与 Noto 主字体不同，同基线下图标比文字**低约 1.3px**
     * （3x 截图像素实测 1.2~1.3px）。translate_y -1 补偿后视觉中心对齐。 */
    lv_obj_t *icon_lbl = plain_label(btn, icon, LVGL_PORT_FONT_16, COL_PRIMARY, false);
    lv_obj_set_style_translate_y(icon_lbl, -1, 0);
    plain_label(btn, name, LVGL_PORT_FONT_16, COL_INK, false);

    lv_obj_add_event_cb(btn, cell_clicked_cb, LV_EVENT_CLICKED, (void *)cb);
    return btn;
}

void screen_settings_create(lv_obj_t *tile)
{
    s_tile = tile;
    s_subpage = NULL;
    s_sub_content = NULL;
    s_sub_kind = SUB_NONE;

    lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(tile, false); /* 网格一屏尽收，不需要滚动 */

    /* 标题 */
    lv_obj_t *title = plain_label(tile, "设置", LVGL_PORT_FONT_20, COL_INK, true);
    lv_obj_set_pos(title, GRID_SIDE_PAD, 24);

    /* 2×3 网格 */
    lv_obj_t *grid = lv_obj_create(tile);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), 3 * CELL_HEIGHT + 2 * CELL_GAP_Y);
    lv_obj_set_pos(grid, 0, GRID_TOP_Y);
    lv_obj_set_style_pad_left(grid, GRID_SIDE_PAD, 0);
    lv_obj_set_style_pad_right(grid, GRID_SIDE_PAD, 0);
    lv_obj_set_style_pad_row(grid, CELL_GAP_Y, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(grid, false);

    static const struct {
        const char *icon;
        const char *name;
        cell_cb_t cb;
    } cells[6] = {
        { LV_SYMBOL_SETTINGS, "测量设置", calib_ui_menu },
        { LV_SYMBOL_IMAGE, "显示设置", display_open_cb },
        { LV_SYMBOL_AUDIO, "蜂鸣器", buzzer_open_cb },
        { LV_SYMBOL_WIFI, "WiFi 与热点", wifi_open_cb },
        { LV_SYMBOL_REFRESH, "电能统计", energy_open_cb },
        { LV_SYMBOL_LIST, "关于本机", about_open_cb },
    };

    for (int r = 0; r < 3; r++) {
        lv_obj_t *row = plain_container(grid);
        lv_obj_set_size(row, LV_PCT(100), CELL_HEIGHT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, CELL_GAP_X, 0);
        make_cell_dispatch(row, cells[r * 2].icon, cells[r * 2].name, cells[r * 2].cb);
        make_cell_dispatch(row, cells[r * 2 + 1].icon, cells[r * 2 + 1].name,
                           cells[r * 2 + 1].cb);
    }
}

void screen_settings_refresh(const data_store_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }
    s_last_snap = *snap;

    switch (s_sub_kind) {
    case SUB_DISPLAY:
        /* 自动旋转开启时，实际角度由重力服务改变，这里让按钮高亮跟随 */
        angle_buttons_refresh();
        break;
    case SUB_CALIB_EDIT:
        if (s_cal_read_label != NULL) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%.5f %s",
                     (double)(s_cal_is_voltage ? snap->voltage_v : snap->current_a),
                     s_cal_is_voltage ? "V" : "A");
            lv_label_set_text(s_cal_read_label, buf);
        }
        break;
    case SUB_ENERGY:
        if (s_energy_wh_label != NULL) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%.5f Wh", (double)snap->energy_wh);
            lv_label_set_text(s_energy_wh_label, buf);
        }
        if (s_energy_rt_label != NULL) {
            char buf[48];
            snprintf(buf, sizeof(buf), "运行时间 %u:%02u:%02u",
                     (unsigned)(snap->runtime_s / 3600u),
                     (unsigned)((snap->runtime_s % 3600u) / 60u),
                     (unsigned)(snap->runtime_s % 60u));
            lv_label_set_text(s_energy_rt_label, buf);
        }
        break;
    default:
        break;
    }
}