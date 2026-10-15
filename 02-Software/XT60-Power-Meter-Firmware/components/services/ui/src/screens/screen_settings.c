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
 * 页面结构（各子页一览）
 * ────────────────────────────────────────────────────────────
 *   首页 2×3 网格 → 点击进入对应二级页：
 *   · 测量设置  → 校准向导（通道选择 → 标准值输入 → 完成提示，三级）
 *   · 显示设置  → 亮度滑条 + 自动旋转开关 + 0/90/180/270° 角度按钮
 *   · 蜂鸣器    → 响度滑条 + 试听按钮
 *   · WiFi      → 路线图说明页（联网能力 v0.5.0 才有，本页不提供开关）
 *   · 电能统计  → 累计电能/运行时间 + 确认框清零
 *   · 关于本机  → 信息对话框（无二级页）
 *
 * ────────────────────────────────────────────────────────────
 * 关键交互约定（全项目 UI 通用，本页严格落实）
 * ────────────────────────────────────────────────────────────
 *   · **纯按键可达**：二级页全部操作都做成按钮/开关/滑条等可聚焦控件，
 *     不依赖触屏手势；"返回"固定在标题栏左上，任何子页都不会把用户困住。
 *     按键操作体系（详见文件尾 screen_settings_key 与"焦点链机制"一节）：
 *     设置首页长按 K2 进入网格焦点模式（单击移焦点、长按 K2 打开焦点格），
 *     任意二级页内单击 K1/K2 移焦点、长按 K2 激活（按钮=点击、开关=切换、
 *     滑条=进/出"调节模式"，调节时 K1/K2 变减/加步进）、长按 K1 返回；
 *     弹窗一律 K1=取消 / K2=确定。全程只需 K1/K2 两个键。
 *   · **写入/清除类操作不把用户留在中间态**：清零等破坏性操作先弹确认框、
 *     完成后弹结果提示（见 show_confirm / show_notice）；校准完成后 1.4s
 *     自动返回首页（见 close_timer_cb），无须用户再点一次"返回"。
 *     电能清零后停留原页，是为了让用户亲眼看到数值归零（约 0.2s 后由
 *     测量核在块边界执行），属有意为之的例外。
 *   · **滑条预留触摸区**：宽度内缩到 84%、主体加高到 24px、旋钮 20×26px，
 *     保证滑到两端时旋钮仍离面板圆角边缘足够远，拇指按得到（防"拖不动"）。
 *   · **屏幕方向用角度表示**（0°=默认横屏，顺时针），切换必须走
 *     lvgl_port_set_rotation_angle()：它在改显示方向的同时**同步触摸输入的
 *     旋转映射**。若只改显示不改触摸，会出现"画面转了但点不准"的错位。
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

#include "button.h"
#include "buzzer.h"
#include "lcd_nv3030b.h"
#include "orientation.h"
#include "power_measure.h"
#include "settings.h"
/* ui_nav：取当前页码（ui_nav_current_page）——设置首页的网格焦点模式需要
 * 知道"用户此刻真的在设置页"，否则按键接管会波及测量/波形页的翻页键位 */
#include "ui_nav.h"

/* ---- 色板（与测量页/波形页同一语义色） ----
 * lv_color_hex(0xRRGGBB)：把网页风格的十六进制色值转换成 LVGL 内部的
 * lv_color_t 颜色结构，之后所有 set_style_*_color 接口都收这种颜色。 */
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
#define CELL_GAP_X    10 /**< 网格两列之间的间距 */
#define CELL_GAP_Y    8  /**< 网格三行之间的间距 */

/* ---- 二级页 ---- */
#define SUB_TOPBAR_H 36 /**< 二级页顶部标题栏高度（含"返回"按钮） */

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
static sub_kind_t s_sub_kind = SUB_NONE; /**< 当前二级页类型（刷新分发的依据） */
static bool s_layer_hooked = false;  /**< 对话框样式与关闭处理是否已安装 */
static lv_obj_t *s_msgbox_scrim = NULL; /**< 当前对话框的压暗遮罩（随框删除） */
static lv_obj_t *s_active_msgbox = NULL; /**< 当前打开的对话框（NULL=无；按键路由据此判模态） */

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
static lv_obj_t *s_cal_target_label = NULL; /**< 标准值显示标签（加减步进后刷新） */

/** 校准步长档位：粗调 → 细调循环切换 */
static const float s_cal_steps[] = { 1.0f, 0.1f, 0.01f, 0.001f };
#define CAL_STEP_COUNT ((int)(sizeof(s_cal_steps) / sizeof(s_cal_steps[0])))

/** 方向按钮（角度按钮高亮当前实际方向） */
static lv_obj_t *s_angle_btns[4] = { 0 }; /**< 0/90/180/270 度按钮 */
static lv_obj_t *s_auto_switch = NULL;    /**< 重力自动旋转开关 */
static const uint16_t s_angle_vals[4] = { 0, 90, 180, 270 };

#define GRID_CELL_COUNT 6 /**< 设置首页网格功能格数 */

/* ---- 设置首页网格焦点（二级页全关时由长按 K2 进入） ---- */
static lv_obj_t *s_grid_cells[GRID_CELL_COUNT] = { 0 }; /**< 六格按钮（登记顺序＝焦点顺序） */
static bool s_grid_focus_on = false; /**< true = 网格焦点模式（单击移焦点/长按 K2 打开焦点格） */
static int s_grid_idx = 0;           /**< 网格焦点格下标（跨子页往返保持，返回后接着来） */

/** 校准完成 1.4s 自动关页定时器（句柄管理：关页/切页时显式删除，防误关新页面） */
static lv_timer_t *s_close_timer = NULL;

/* ============================================================
 * 对话框（lv_msgbox + 顶层图层可点击技巧，沿用原实现）
 * ============================================================ */

/**
 * @brief 点击顶层图层空白处 → 关闭当前对话框（点遮罩关闭）
 *
 * 用**同步** lv_msgbox_close 而非 close_async：按键路径（screen_settings_key
 * → msgbox_key_action）也是同步关闭，两条路径若一同步一异步，异步排队的
 * 回调可能在对象已被另一路径删除后才执行（悬空指针）。统一同步关闭后
 * 无论点遮罩、点按钮还是按 K1/K2，都走同一个立即关闭出口。
 *
 * @param e LVGL 点击事件（目标是顶层图层 lv_layer_top()）
 */
static void layer_clicked_cb(lv_event_t *e)
{
    lv_obj_t *layer = lv_event_get_current_target(e);
    /* lv_obj_get_child_count()：数一数容器里现有几个子对象，用来判断
     * "还有没有对话框开着"（对话框就是顶层图层的最后一个子对象）。 */
    uint32_t n = lv_obj_get_child_count(layer);
    if (n > 0) {
        /* lv_obj_get_child(layer, n-1) 取最后创建的子对象 = 当前对话框 */
        lv_msgbox_close(lv_obj_get_child(layer, n - 1));
    }
}

/**
 * @brief 延迟一帧复查：图层已无子对象则恢复"不可点击"
 *
 * 时序说明：LV_EVENT_DELETE 触发时，被删对象**可能仍留在父对象的子列表中**，
 * 此刻直接查 child_count 会拿到非 0 值，导致顶层图层永远保持 clickable、
 * 把后续所有触摸全部吞掉（表现为"对话框关掉后返回按钮点不动"）。
 * 因此把复查放到下一帧（lv_async_call）执行。
 *
 * @param layer_ptr 顶层图层指针（void* 形参是为了匹配 lv_async_call 的回调签名）
 */
static void layer_restore_check_cb(void *layer_ptr)
{
    lv_obj_t *layer = (lv_obj_t *)layer_ptr;
    if (layer != NULL && lv_obj_get_child_count(layer) == 0) {
        /* lv_obj_set_clickable(false)：让对象不再参与触摸命中测试，
         * 触摸会"穿过"它落到下层页面控件上（否则整个页面被图层吞掉点击）。 */
        lv_obj_set_clickable(layer, false);
    }
}

/**
 * @brief 对话框被删除 → 删除压暗遮罩 + 安排一次延迟复查（恢复图层不可点击）
 *
 * 同时清 s_active_msgbox：弹窗句柄随对象一起失效，按键路由（screen_settings_key）
 * 靠它判断"此刻有没有弹窗要接管按键"，不清理会一直误判为模态。
 *
 * @param e LVGL 删除事件；user_data 携带顶层图层指针
 */
static void box_deleted_cb(lv_event_t *e)
{
    lv_obj_t *box = lv_event_get_target(e); /* 被删的对话框本体 */
    lv_obj_t *layer = (lv_obj_t *)lv_event_get_user_data(e);
    if (s_active_msgbox == box) {
        s_active_msgbox = NULL;
    }
    if (s_msgbox_scrim != NULL) {
        /* lv_obj_delete()：立即删除遮罩对象（连同其样式一起释放） */
        lv_obj_delete(s_msgbox_scrim);
        s_msgbox_scrim = NULL;
    }
    if (layer != NULL) {
        /* lv_async_call()：把复查函数挂到下一帧的异步队列执行，
         * 避开"删除事件触发时对象还在子列表里"的时序陷阱（见上方注释） */
        lv_async_call(layer_restore_check_cb, layer);
    }
}

/**
 * @brief 把对话框里的文字全部换成中文字体（主题字体已是 Noto，这里是双保险）
 *
 * @param box lv_msgbox_create 创建的对话框对象
 */
static void style_msgbox(lv_obj_t *box)
{
    /* lv_msgbox_get_title / get_content / get_footer：分别取对话框的
     * 标题栏、正文区、底部按钮区三段的子对象（LVGL 9 的 msgbox 由这三块组成）。 */
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

/**
 * @brief 创建挂到顶层图层上的模态框（公共部分）
 *
 * @param title 标题文本
 * @param text  正文文本
 * @return 创建好的 lv_msgbox 对象（已加标题/正文并居中，按钮由调用方补）
 */
static lv_obj_t *msgbox_begin(const char *title, const char *text)
{
    /* lv_layer_top()：LVGL 的"顶层图层"，永远画在所有页面之上。
     * 对话框挂在这里，翻页/滚动都不会把它带走，天然呈现模态效果。 */
    lv_obj_t *layer = lv_layer_top();

    /* 关键：让顶层图层参与命中测试，否则里面的按钮点不动（见原实现注释） */
    lv_obj_set_clickable(layer, true);
    if (!s_layer_hooked) {
        s_layer_hooked = true;
        lv_obj_add_event_cb(layer, layer_clicked_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 压暗遮罩：先于对话框创建（渲染在下层），模态感 + 突出对话框。
     * 不可点击 → 命中测试穿透到顶层图层本体，点遮罩空白处仍走
     * layer_clicked_cb 关闭对话框（与原交互一致）。
     * lv_obj_remove_style_all()：清掉主题默认样式，得到一块干净的"画布"；
     * lv_obj_set_style_bg_opa(..., LV_OPA_40, 0) 把黑色底调到 40% 不透明度，
     * 即"压暗 60%"，视觉上提示"背后页面暂时不可用"。 */
    lv_obj_t *scrim = lv_obj_create(layer);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100)); /* LV_PCT(100)：占父对象满宽/满高 */
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_40, 0);
    lv_obj_set_clickable(scrim, false);
    s_msgbox_scrim = scrim;

    /* lv_msgbox_create()：LVGL 9 内置对话框部件，自带标题/正文/按钮区三段结构 */
    lv_obj_t *box = lv_msgbox_create(layer);
    lv_obj_set_width(box, 250); /* 固定宽度，避免长文本撑到圆角外 */
    lv_msgbox_add_title(box, title); /* 往对话框里追加标题段 */
    lv_msgbox_add_text(box, text);   /* 往对话框里追加正文段 */
    style_msgbox(box);
    lv_obj_center(box); /* 相对父对象（顶层图层）居中 */

    /* 对话框删除后恢复图层可点击性 */
    lv_obj_add_event_cb(box, box_deleted_cb, LV_EVENT_DELETE, layer);
    s_active_msgbox = box; /* 记录当前弹窗：按键路由据此把 K1/K2 让给弹窗 */
    return box;
}

static void notice_ok_cb(lv_event_t *e); /* 前向声明（定义见下方） */

/**
 * @brief 单按钮提示框（"确定"，点击显式关闭）
 *
 * @param title 标题文本
 * @param text  正文文本
 */
static void show_notice(const char *title, const char *text)
{
    lv_obj_t *box = msgbox_begin(title, text);
    /* lv_msgbox_add_footer_button()：在对话框底部按钮区追加一个按钮 */
    lv_obj_t *ok = lv_msgbox_add_footer_button(box, "确定");
    lv_obj_add_event_cb(ok, notice_ok_cb, LV_EVENT_CLICKED, NULL); /* 点击触发关闭 */
    style_msgbox(box); /* footer 按钮后补的子对象也要过一遍字体设置 */
}

/** 确认框的"确定"动作回调 */
typedef void (*confirm_cb_t)(void);

/**
 * @brief 从 footer 按钮向上找到所属 msgbox 并**显式关闭**（幂等）
 *
 * 防御式设计：对话框的关闭不依赖 LVGL footer 按钮的内建关闭行为——
 * 若该行为在某种情况下失效，对话框将一直留在顶层图层上，把后续所有触摸
 * 全部吞掉（表现为"对话框关不掉、返回按钮也点不动"）。显式关闭双保险。
 *
 * @param btn 被点击的 footer 按钮（沿父链向上找到所属 msgbox）
 */
static void msgbox_close_from_btn(lv_obj_t *btn)
{
    /* 从按钮一路向上找父对象，直到命中 lv_msgbox 的类对象为止。
     * lv_obj_check_type(o, &lv_msgbox_class)：判断对象是不是 msgbox 类型；
     * lv_obj_get_parent(o)：取直接父对象。 */
    lv_obj_t *o = btn;
    while (o != NULL && !lv_obj_check_type(o, &lv_msgbox_class)) {
        o = lv_obj_get_parent(o);
    }
    if (o != NULL) {
        /* lv_msgbox_close(o)：同步关闭（立即删除）该对话框 */
        lv_msgbox_close(o);
    }
}

/** @brief 单按钮提示框："确定"点击 → 显式关闭
 *  @param e LVGL 点击事件（目标是"确定"按钮） */
static void notice_ok_cb(lv_event_t *e)
{
    msgbox_close_from_btn(lv_event_get_target(e)); /* lv_event_get_target：取触发事件的对象 */
}

/**
 * @brief 确认框"确定"点击 → 先关本框，再执行业务动作
 *
 * @param e LVGL 点击事件；user_data 携带业务回调 confirm_cb_t
 */
static void confirm_ok_cb(lv_event_t *e)
{
    /* lv_event_get_user_data()：取出注册事件时塞进去的自定义指针（这里是业务回调） */
    confirm_cb_t cb = (confirm_cb_t)lv_event_get_user_data(e);
    /* 先关闭本框（同步删除，立即生效），再执行业务动作（动作可能弹新框） */
    msgbox_close_from_btn(lv_event_get_target(e));
    if (cb != NULL) {
        cb();
    }
}

/**
 * @brief 确认框（取消 / 确定）
 *
 * @param title  标题文本
 * @param text   正文文本（说明后果，含"不可撤销"提示）
 * @param ok_cb  点"确定"后执行的业务动作；点"取消"直接关框不动作
 */
static void show_confirm(const char *title, const char *text, confirm_cb_t ok_cb)
{
    lv_obj_t *box = msgbox_begin(title, text);
    /* ⚠ LVGL 9.6 的 footer 按钮**没有**内建关闭行为（只有 add_close_button 才挂
     * 关闭回调），"取消"必须显式挂关闭回调，否则点它/按 K1 都关不掉框。
     * notice_ok_cb 的语义就是"关框不动作"，与"取消"一致，直接复用。 */
    lv_obj_t *cancel = lv_msgbox_add_footer_button(box, "取消");
    lv_obj_add_event_cb(cancel, notice_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok = lv_msgbox_add_footer_button(box, "确定");
    lv_obj_add_event_cb(ok, confirm_ok_cb, LV_EVENT_CLICKED, (void *)ok_cb); /* 业务回调经 user_data 传入 */
    style_msgbox(box);
}

/* ============================================================
 * 通用小部件
 * ============================================================ */

/**
 * @brief 透明容器（去样式，不改 flex 行为）
 *
 * 用作布局"行/块"：只负责排版，自己不显示任何东西。
 * @param parent 父对象
 * @return 创建好的容器对象
 */
static lv_obj_t *plain_container(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent); /* lv_obj_create：最基础的"空白矩形"对象 */
    lv_obj_remove_style_all(o);          /* 去掉主题样式：无背景/边框/内边距 */
    lv_obj_set_scrollable(o, false);     /* 不可滚动：纯布局容器不响应滑动 */
    return o;
}

/**
 * @brief 文本标签（指定字号/颜色/字重）
 *
 * @param parent 父对象
 * @param text   显示文本（LVGL 内部按 UTF-8 处理，中文字体由 lvgl_port 提供）
 * @param size   字号枚举（映射到 lvgl_port 的中文字体）
 * @param color  文字颜色
 * @param bold   true=粗体字体，false=常规字体
 * @return 创建好的标签对象
 */
static lv_obj_t *plain_label(lv_obj_t *parent, const char *text, lvgl_port_font_size_t size,
                             lv_color_t color, bool bold)
{
    lv_obj_t *l = lv_label_create(parent); /* lv_label_create：创建文本标签部件 */
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, bold ? lvgl_port_get_font_bold(size)
                                       : lvgl_port_get_font(size),
                               0); /* 设置字体（中文字体按字重二选一） */
    lv_obj_set_style_text_color(l, color, 0); /* 文字颜色 */
    return l;
}

/**
 * @brief 通用主按钮（白底描边，按下态浅蓝）
 *
 * 尺寸 ≥36px 高是触摸目标下限（手指约 7~10mm），刻意不用更小的按钮。
 * @param parent 父对象
 * @param text   按钮文字
 * @param w,h    宽/高（px，或 LV_PCT(n) 表示父对象百分比）
 * @return 创建好的按钮对象（文字标签为其第 0 个子对象）
 */
static lv_obj_t *plain_button(lv_obj_t *parent, const char *text, int32_t w, int32_t h)
{
    /* lv_button_create：LVGL 9 按钮部件（自带按压反馈，样式可覆盖） */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    /* LV_STATE_PRESSED 是"状态选择器"：同一属性可按状态分别设值——
     * 第 4 参传 0 是默认态，传 LV_STATE_PRESSED 是"被按下时"才生效的样式 */
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_color(btn, COL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0); /* COVER=完全不透明（默认透明会导致看不到底色） */
    lv_obj_set_style_border_color(btn, COL_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);   /* 1px 边框让按钮在白底上可辨 */
    lv_obj_set_style_radius(btn, 8, 0);         /* 8px 圆角 */
    lv_obj_set_style_shadow_width(btn, 0, 0);   /* 去掉主题自带阴影，保持扁平风格 */

    lv_obj_t *lbl = plain_label(btn, text, LVGL_PORT_FONT_16, COL_INK, false);
    lv_obj_center(lbl); /* 文字相对按钮居中 */
    return btn;
}

/* ============================================================
 * 纯按键焦点链机制（P0-2：设置页全部操作脱离触屏）
 * ────────────────────────────────────────────────────────────
 * 设计取舍：不引入 LVGL 的 lv_group + KEYPAD 焦点体系——那要求给每个控件
 * 建 group、配 indev 焦点遍历，且与本项目"按键事件经 ui_nav 按页路由"
 * （ui_nav_handle_key）的架构冲突。这里用一张**焦点链**（数组登记本页
 * 可操作控件 + 当前下标）实现同等能力：新子页只需 focus_begin/focus_add
 * 登记，按键逻辑全页复用，不必每个子页手写一套。
 *
 * 键位（与测量/波形页快速菜单同一心智，见 screen_settings.h 契约）：
 *   单击 K1/K2  = 焦点上/下移动（环形）；
 *   长按 K2     = 激活焦点（按钮=点击；开关=切换；滑条=进"调节模式"）；
 *   长按 K1     = 返回/关闭当前子页（等效触屏"返回"按钮）；
 *   滑条调节模式：单击 K1/K2=减/加步进、长按 K2=退出并落盘、长按 K1=落盘并返回。
 * 弹窗另有固定键位（K1=取消、K2=确定），优先级高于焦点链（见 screen_settings_key）。
 * 按键反馈鸣叫由 ui_nav_handle_key 统一播放，本文件不重复响。
 * ============================================================ */

#define FOCUS_MAX 8   /**< 焦点链容量（最长子页 6 控件，留余量） */
#define SLIDER_STEP 5 /**< 滑条按键步进（百分点：10~100 档 20 步走完） */

static lv_obj_t *s_focus_widgets[FOCUS_MAX]; /**< 焦点链：当前子页可操作控件 */
static int s_focus_cnt = 0;                  /**< 焦点链有效长度 */
static int s_focus_idx = 0;                  /**< 当前焦点下标 */
static lv_obj_t *s_focus_back = NULL;        /**< 本页"返回"按钮（长按 K1 等效点击它） */
static bool s_slider_edit = false; /**< true=滑条调节模式（K1/K2 变减/加步进） */

/**
 * @brief 重绘焦点高亮（当前控件 2px 蓝框；滑条调节中改绿框示意）
 *
 * 用 **outline（轮廓）** 而非 border：outline 画在对象边框外侧、不参与布局
 * 计算，加/去焦点框不会挤动周围控件（border_width 变化会改变布局尺寸）。
 * 调节模式下焦点框变绿加粗：用户一眼看出"K1/K2 此刻是加减而非移焦点"。
 */
static void focus_paint(void)
{
    bool edit = s_slider_edit && s_focus_cnt > 0 &&
                lv_obj_check_type(s_focus_widgets[s_focus_idx], &lv_slider_class);
    for (int i = 0; i < s_focus_cnt; i++) {
        lv_obj_t *w = s_focus_widgets[i];
        if (w == NULL) {
            continue;
        }
        bool on = (i == s_focus_idx);
        lv_obj_set_style_outline_width(w, on ? (edit ? 3 : 2) : 0, 0);
        lv_obj_set_style_outline_color(w, (edit && on) ? COL_OK : COL_PRIMARY, 0);
        lv_obj_set_style_outline_pad(w, 2, 0);
    }
}

/**
 * @brief 重绘首页网格焦点高亮（焦点格 2px 蓝框，其余无框）
 *
 * 与 focus_paint 同款 outline 方案：不参与布局，加框不挤动网格。
 */
static void grid_paint(void)
{
    for (int i = 0; i < GRID_CELL_COUNT; i++) {
        lv_obj_t *w = s_grid_cells[i];
        if (w == NULL) {
            continue;
        }
        bool on = s_grid_focus_on && (i == s_grid_idx);
        lv_obj_set_style_outline_width(w, on ? 2 : 0, 0);
        lv_obj_set_style_outline_color(w, COL_PRIMARY, 0);
        lv_obj_set_style_outline_pad(w, 2, 0);
    }
}

/**
 * @brief 焦点链清空（子页关闭/重建时调用，防悬空指针）
 *
 * 只降计数不擦数组：焦点链里的对象指针随子页 lv_obj_delete 一起失效，
 * 留着旧值无害（paint/移动只遍历 0..s_focus_cnt-1）。
 */
static void focus_end(void)
{
    s_focus_cnt = 0;
    s_focus_idx = 0;
    s_focus_back = NULL;
    s_slider_edit = false;
}

/**
 * @brief 焦点链初始化：记录"返回"按钮，链从空开始
 * @param back 本页标题栏"返回"按钮（长按 K1 等效点击它；可为 NULL）
 */
static void focus_begin(lv_obj_t *back)
{
    focus_end();
    s_focus_back = back;
}

/**
 * @brief 登记一个可操作控件到焦点链（按用户操作顺序调用）
 * @param w 控件对象（NULL 或链满时忽略）
 */
static void focus_add(lv_obj_t *w)
{
    if (w == NULL || s_focus_cnt >= FOCUS_MAX) {
        return;
    }
    s_focus_widgets[s_focus_cnt++] = w;
    /* 每登记一项立即重画焦点框：子页建完的最后一项登记后，焦点框已就位
     * （焦点默认停在第 0 项），纯按键用户一进页面就看得见"操作对象是谁"。
     * 建页是一次性动作，6 项以内的样式重设开销可忽略。 */
    focus_paint();
}

/**
 * @brief 焦点环形移动
 * @param dir +1=下一项 / -1=上一项（负数取模先加长度防溢出）
 */
static void focus_move(int dir)
{
    if (s_focus_cnt <= 0) {
        return;
    }
    s_focus_idx = (s_focus_idx + dir + s_focus_cnt) % s_focus_cnt;
    focus_paint();
    /* lv_obj_scroll_to_view：焦点控件若被内容区滚动裁掉，自动滚到可见
     * （LV_ANIM_ON 平滑滚动）；已可见时无动作。 */
    lv_obj_scroll_to_view(s_focus_widgets[s_focus_idx], LV_ANIM_ON);
}

/**
 * @brief 滑条调节模式：按方向步进一个 SLIDER_STEP
 *
 * ⚠ LVGL 的 lv_bar_set_value/lv_slider_set_value **不会**派发 VALUE_CHANGED
 * （那是触摸拖动路径才发的事件），因此步进后必须手动 lv_obj_send_event
 * 补发一次，否则亮度/响度回调不执行——滑条看起来动了但背光/音量没变。
 *
 * @param dir -1=减 / +1=加（自动夹到 [min,max]）
 */
static void slider_step(int dir)
{
    if (s_focus_cnt <= 0) {
        return;
    }
    lv_obj_t *w = s_focus_widgets[s_focus_idx];
    if (!lv_obj_check_type(w, &lv_slider_class)) {
        return;
    }
    int32_t v = lv_slider_get_value(w) + (int32_t)dir * SLIDER_STEP;
    /* lv_slider_get_min_value/get_max_value：读滑动条合法范围（建页时 set_range 设定） */
    int32_t lo = lv_slider_get_min_value(w);
    int32_t hi = lv_slider_get_max_value(w);
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    lv_slider_set_value(w, v, LV_ANIM_ON);
    lv_obj_send_event(w, LV_EVENT_VALUE_CHANGED, NULL); /* 手动补发：立即生效 + 刷数值标签 */
}

/**
 * @brief 退出滑条调节模式：补发 RELEASED 让回调落盘
 *
 * 亮度/响度回调的策略是"拖动中只改内存缓存、松手才 settings_save_now()"
 * （省 NVS 擦写寿命）。按键路径没有真实"松手"，退出调节时合成一次
 * RELEASED 事件即可复用同一落盘逻辑（lv_event_get_user_data 在合成事件里
 * 会取到注册回调时的 user_data，回调行为与触摸路径完全一致）。
 */
static void slider_edit_exit(void)
{
    if (s_focus_cnt <= 0) {
        return;
    }
    lv_obj_t *w = s_focus_widgets[s_focus_idx];
    s_slider_edit = false;
    lv_obj_send_event(w, LV_EVENT_RELEASED, NULL); /* 触发 settings_save_now() 落盘 */
    focus_paint();
}

/**
 * @brief 激活当前焦点控件（长按 K2）
 *
 * 按控件类型分派（lv_obj_check_type 比对 LVGL 控件类对象）：
 *   · 滑条  → 进入调节模式（后续单击=减/加，长按 K2=退出）；
 *   · 开关  → 翻转 CHECKED 状态并补发 VALUE_CHANGED（回调读状态执行业务）；
 *   · 其余（按钮/msgbox footer 按钮）→ 合成一次 LV_EVENT_CLICKED。
 * 合成 CLICKED 是可靠的：lv_event_send 会把注册回调时的 user_data 注入事件，
 * 业务回调拿到的参数与触摸点击完全一致（如通道按钮的通道标志、确认框的 ok_cb）。
 */
static void focus_activate(void)
{
    if (s_focus_cnt <= 0) {
        return;
    }
    lv_obj_t *w = s_focus_widgets[s_focus_idx];
    if (lv_obj_check_type(w, &lv_slider_class)) {
        s_slider_edit = true;
        focus_paint();
        return;
    }
    if (lv_obj_check_type(w, &lv_switch_class)) {
        /* lv_obj_has_state/add_state/remove_state：读/置对象状态位。
         * 开关的 VALUE_CHANGED 只在状态翻转时由业务回调关心，故手动补发。 */
        if (lv_obj_has_state(w, LV_STATE_CHECKED)) {
            lv_obj_remove_state(w, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(w, LV_STATE_CHECKED);
        }
        lv_obj_send_event(w, LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }
    lv_obj_send_event(w, LV_EVENT_CLICKED, NULL);
}

/* ============================================================
 * 二级页框架
 * ============================================================ */

/**
 * @brief 关闭当前二级页（幂等）
 *
 * 除了删除浮层，还要把"随页而生"的刷新标签指针全部清 NULL——这些指针
 * 指向的对象已随浮层被 lv_obj_delete() 连带删除，若不置空，下一帧
 * screen_settings_refresh() 还会去写它们，形成野指针崩溃。
 * 焦点链与"自动关页定时器"同理：对象没了/页没了，登记的状态必须一并失效。
 */
static void subpage_close(void)
{
    /* 自动关页定时器：页都关了必须删掉，否则 1.4s 后回调会去关一个
     * 不存在（或已被新页面占用）的浮层（任务 2 的根因之一）。 */
    if (s_close_timer != NULL) {
        lv_timer_delete(s_close_timer); /* lv_timer_delete：从 LVGL 定时器链表摘除并释放 */
        s_close_timer = NULL;           /* 置 NULL 防悬空：删完的句柄绝不能再 delete */
    }
    if (s_subpage != NULL) {
        /* lv_obj_delete()：删除对象并连带删除其所有子对象（标题栏/内容区在内） */
        lv_obj_delete(s_subpage);
    }
    s_subpage = NULL;
    s_sub_content = NULL;
    s_sub_kind = SUB_NONE;
    focus_end(); /* 焦点链指针随浮层失效，清空下标/调节模式 */
    s_cal_read_label = NULL;
    s_cal_target_label = NULL;
    s_energy_wh_label = NULL;
    s_energy_rt_label = NULL;
    s_bright_value_label = NULL;
    s_vol_value_label = NULL;
    memset(s_angle_btns, 0, sizeof(s_angle_btns));
    s_auto_switch = NULL;
}

/**
 * @brief 二级页"返回"按钮点击 → 关闭当前二级页
 *
 * 项目约定"纯按键可达"：返回入口固定在标题栏左上角，位置在所有二级页一致，
 * 用户闭着眼也能找到退路（不依赖触屏手势）。
 *
 * @param e LVGL 点击事件（未使用）
 */
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
    /* 首页网格焦点模式与二级页互斥：一旦进子页就退出网格模式并去掉格子高亮，
     * 否则子页返回后单击仍会"无感地"挪网格焦点，用户困惑。 */
    if (s_grid_focus_on) {
        s_grid_focus_on = false;
        grid_paint();
    }

    s_subpage = lv_obj_create(s_tile);
    lv_obj_remove_style_all(s_subpage);
    lv_obj_set_size(s_subpage, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_subpage, 0, 0);
    lv_obj_set_style_bg_color(s_subpage, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_subpage, LV_OPA_COVER, 0);
    /* lv_obj_set_scroll_dir(..., LV_DIR_ALL)：允许"四向滚动"——内容即使不溢出
     * 也把滑动手势吸收掉，手势不会冒泡到外层 tileview 造成误翻页 */
    lv_obj_set_scroll_dir(s_subpage, LV_DIR_ALL); /* 吞掉滑动，防误翻页 */
    lv_obj_set_scrollbar_mode(s_subpage, LV_SCROLLBAR_MODE_OFF); /* 关掉滚动条（不可见但仍在吸收手势） */
    /* 垂直 flex 排布：标题栏在上、内容区占满剩余高度。两块区域**不重叠**，
     * 内容容器永远不会盖住"返回"按钮（曾因全屏覆盖导致返回键点不到）。
     * 顶部 22px 内缩让开共享状态栏。
     * lv_obj_set_style_pad_*：内边距（pad_top 顶部 / pad_row 子对象纵向间距）；
     * lv_obj_set_flex_flow(COLUMN)：开启 flex 纵向排版，子对象从上往下排。 */
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
    lv_obj_set_size(back, 72, 28);                  /* 72×28：容得下"< 返回"且可点中 */
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 2, 0);    /* lv_obj_align：相对父对象（标题栏）左端垂直居中 */
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
    /* 焦点链：每个二级页的"返回"按钮都是长按 K1 的落点（见焦点链机制一节）。
     * 可操作控件由各子页建好后自行 focus_add 登记，顺序即遍历顺序。 */
    focus_begin(back);

    lv_obj_t *tl = plain_label(bar, title, LVGL_PORT_FONT_16, COL_INK, true);
    lv_obj_align(tl, LV_ALIGN_CENTER, 0, 0);

    /* ---- 内容容器（flex 剩余高度，从标题栏下方开始，不与其重叠） ---- */
    lv_obj_t *c = lv_obj_create(s_subpage);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, 0);       /* 高度交给 flex 分配（配合 flex_grow） */
    lv_obj_set_flex_grow(c, 1); /* 占满标题栏以下的剩余高度（flex_grow=1：独占余量） */
    lv_obj_set_style_pad_top(c, 8, 0); /* 与标题栏的间距 */
    lv_obj_set_style_pad_hor(c, 14, 0); /* 左右内缩 14px：避开面板圆角 */
    lv_obj_set_style_pad_bottom(c, 10, 0);
    lv_obj_set_style_pad_row(c, 8, 0); /* 内部子对象纵向间距 8px */
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    /* lv_obj_set_flex_align：分别设主轴/交叉轴/整体对齐方式（这里统一居中） */
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(c, LV_DIR_VER); /* 内容区只允许纵向滚动（长内容上下滑） */
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_AUTO); /* 滚动条按需自动显示 */

    s_sub_content = c;
    return c;
}

/* ============================================================
 * 二级页：测量设置（标定向导）
 * ============================================================ */

static void calib_ui_enter(void); /* 前向声明 */

/**
 * @brief 点击"电压校准/电流校准"按钮 → 进入校准编辑界面
 *
 * @param e LVGL 点击事件；user_data 携带通道标志（1=电压，0=电流）
 */
static void calib_channel_clicked_cb(lv_event_t *e)
{
    /* 按钮不新建回调，而是把通道标志塞进 user_data 传过来，两个按钮共用一个回调 */
    s_cal_is_voltage = (bool)(intptr_t)lv_event_get_user_data(e);
    calib_ui_enter();
}

/**
 * @brief 通道选择页（测量设置的首页）
 *
 * 展示当前增益摘要 + 两个通道入口按钮。
 */
static void calib_ui_menu(void)
{
    lv_obj_t *c = subpage_open(SUB_CALIB_MENU, "测量设置");

    /* 顶部说明文字：告诉用户"校准是拿标准表对照修增益"，降低理解门槛 */
    lv_obj_t *hint = plain_label(c,
                                 "用标准表对照修正读数（单点增益校准）：\n"
                                 "挂上已知负载，输入标准表读数后按“应用”。",
                                 LVGL_PORT_FONT_12, COL_GRAY, false);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    /* 当前标定摘要卡片：把抽象的增益系数亮给用户看，校准前后可对比 */
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

    /* 焦点链：电压 → 电流（用户操作顺序；纯按键走这两格即可进校准） */
    focus_add(bv);
    focus_add(bi);
}

/** @brief 校准页数值/步长变化时刷新目标值显示
 *
 *  @note 目标值标签可能已被关闭（NULL），必须判空再写，防止野指针。 */
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

/**
 * @brief "步长"按钮点击 → 粗调/细调档位循环切换
 *
 * 直接改按钮自己的文字标签（按钮的第 0 个子对象），无须额外记标签指针。
 *
 * @param e LVGL 点击事件（目标是"步长"按钮）
 */
static void calib_step_cycle_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    s_cal_step_idx = (s_cal_step_idx + 1) % CAL_STEP_COUNT; /* 取模循环：1.0→0.1→0.01→0.001→1.0 */
    char buf[32];
    snprintf(buf, sizeof(buf), "步长 %.3f", (double)s_cal_steps[s_cal_step_idx]);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0); /* 按钮内第 0 个子对象 = 文字标签 */
    if (lbl != NULL) {
        lv_label_set_text(lbl, buf);
    }
}

/**
 * @brief "－"按钮点击：目标值减一个步长（不小于 0）
 *
 * @param e LVGL 点击事件（未使用）
 */
static void calib_minus_cb(lv_event_t *e)
{
    (void)e;
    s_cal_target -= s_cal_steps[s_cal_step_idx];
    if (s_cal_target < 0.0f) {
        s_cal_target = 0.0f; /* 标准值不设负数：物理量按幅值校准 */
    }
    calib_target_update_label();
}

/**
 * @brief "＋"按钮点击：目标值加一个步长
 *
 * @param e LVGL 点击事件（未使用）
 */
static void calib_plus_cb(lv_event_t *e)
{
    (void)e;
    s_cal_target += s_cal_steps[s_cal_step_idx];
    calib_target_update_label();
}

/**
 * @brief 关闭二级页的一次性定时器（校准完成提示后自动返回）
 *
 * 对应项目约定"完成后自动关闭子菜单"：校准成功已无待办操作，
 * 自动返回首页，用户无须再点一次"返回"。
 *
 * 【任务 2 修复】原实现在 1.4s 后无条件 subpage_close()：若这 1.4s 内用户
 * 手动切去了别的子页（或已回到首页再开了新页），到点会把**无辜的新页面**
 * 关掉。现在定时器经 user_data 携带"启动时的目标子页类型"，回调先比对
 * 当前 s_sub_kind，不匹配说明页面已换，直接作废定时器不动任何页面。
 *
 * @param t 触发的定时器（用完即删，一次性）
 */
static void close_timer_cb(lv_timer_t *t)
{
    /* lv_timer_get_user_data()：取出创建定时器时存入的用户上下文
     * （这里用 (sub_kind_t)(intptr_t) 编码目标子页类型） */
    sub_kind_t want = (sub_kind_t)(intptr_t)lv_timer_get_user_data(t);
    lv_timer_delete(t); /* 一次性定时器：触发后自删，防止重复触发 */
    s_close_timer = NULL; /* 句柄已失效，置 NULL 防 subpage_close 二次删除 */
    if (s_sub_kind != want) {
        /* 目标页面早已不在（用户已手动切页/返回）：什么都不关 */
        return;
    }
    subpage_close();
}

/**
 * @brief 应用校准：new_gain = old_gain × (标准值 / 当前读数)
 *
 * 两道防呆：读数过小（未接负载）→ 拒绝；比值超出 [0.5, 2]（输入错位数）
 * → 拒绝。避免一次误操作把增益改飞、越修越偏。
 *
 * @param e LVGL 点击事件（未使用）
 */
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
        /* 防呆：比值超 [0.5, 2] 说明标准值/读数对不上（多半是输错量级），
         * 此时宁可拒绝也不能把增益一次改飞 */
        show_notice("偏差过大", "标准值与读数相差超过 2 倍，\n请检查输入是否正确。");
        buzzer_play(BUZZER_TONE_ERROR);
        return;
    }

    /* 增益是乘性修正：只改当前通道的 gain，零点 offset 保持原值 */
    power_measure_calib_t cal;
    power_measure_get_calibration(&cal);
    if (s_cal_is_voltage) {
        cal.voltage_gain *= ratio;
    } else {
        cal.current_gain *= ratio;
    }
    if (power_measure_set_calibration(&cal) != ESP_OK) {
        /* 写入失败多半是 NVS 异常，提示重试而不是静默吞掉 */
        show_notice("校准失败", "参数保存失败，请重试。");
        buzzer_play(BUZZER_TONE_ERROR);
        return;
    }

    buzzer_play(BUZZER_TONE_OK);

    /* 结果提示（同页居中显示，1.4s 后自动返回首页——见 close_timer_cb） */
    lv_obj_t *c = subpage_open(SUB_CALIB_DONE, "测量设置");
    char buf[64];
    snprintf(buf, sizeof(buf), "校准完成\n增益已修正 ×%.4f", (double)ratio);
    lv_obj_t *done = plain_label(c, buf, LVGL_PORT_FONT_16, COL_OK, true);
    lv_obj_set_style_text_align(done, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(done, 24, 0); /* margin_top：外边距（与内边距 pad 不同，撑开与上文距离） */

    /* lv_timer_create：1400ms 后触发一次自动关页。
     * ① 句柄存 s_close_timer：subpage_close() 会在用户提前切页/返回时显式删掉它
     *    （防"新页面被旧定时器误关"，见 close_timer_cb 注释）；
     * ② user_data 携带目标子页类型 SUB_CALIB_DONE：回调据此校验"要关的还是
     *    当初那一页"，不匹配直接作废（任务 2 的双重保险）。 */
    s_close_timer = lv_timer_create(close_timer_cb, 1400, (void *)(intptr_t)SUB_CALIB_DONE);
}

/**
 * @brief 电流零点一键清零（空载校零）
 *
 * 输出空载时点击：把电流零偏 offset 调到「抵消当前读数」的位置，
 * 使显示立即归零。物理量 = raw×gain + offset，故
 *   offset_new = offset_old − 当前读数
 * 之后再施加负载，读数即从 0 起算（消除前端零点漂移）。
 *
 * @param e LVGL 点击事件（未使用）
 */
static void calib_zero_cb(lv_event_t *e)
{
    (void)e;
    /* power_measure_get/set_calibration：读/写测量核的标定参数（增益 + 零偏） */
    power_measure_calib_t cal;
    power_measure_get_calibration(&cal);
    cal.current_offset -= s_last_snap.current_a;
    if (power_measure_set_calibration(&cal) != ESP_OK) {
        show_notice("清零失败", "参数保存失败，请重试。");
        return;
    }
    show_notice("已清零", "电流零点已校准。\n（空载读数归零）");
}

/**
 * @brief 校准编辑界面（标准值输入）
 *
 * 排布刻意紧凑：保证"应用校准"按钮首屏可见，无须滚动即可完成全流程
 * （纯按键/小屏友好）。
 */
static void calib_ui_enter(void)
{
    lv_obj_t *c = subpage_open(SUB_CALIB_EDIT, s_cal_is_voltage ? "校准 · 电压" : "校准 · 电流");

    /* 目标值以当前读数为初始值（用户在此基础上按标准表修正） */
    float reading = s_cal_is_voltage ? s_last_snap.voltage_v : s_last_snap.current_a;
    s_cal_target = fabsf(reading);
    s_cal_step_idx = s_cal_is_voltage ? 2 : 3; /* 电压 0.01，电流 0.001（电流读数小，步长更细） */

    /* 紧凑排布：确保"应用校准"按钮首屏可见（无须滚动） */
    lv_obj_set_style_pad_row(c, 6, 0);

    /* 当前读数（实时刷新）：大号品牌蓝，是用户校准时的"对照焦点" */
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

    /* 步长切换：一个按钮循环 4 档（1.0→0.1→0.01→0.001），
     * 比放 4 个步长按钮省空间，且符合"先粗调后微调"的校准习惯 */
    char step_buf[32];
    snprintf(step_buf, sizeof(step_buf), "步长 %.3f", (double)s_cal_steps[s_cal_step_idx]);
    lv_obj_t *step = plain_button(c, step_buf, LV_PCT(60), 30);
    lv_obj_add_event_cb(step, calib_step_cycle_cb, LV_EVENT_CLICKED, NULL);

    /* 电流通道：一键清零（空载校零，输出空载时抵消当前读数） */
    lv_obj_t *zero = NULL;
    if (!s_cal_is_voltage) {
        zero = plain_button(c, "一键清零（空载）", LV_PCT(100), 36);
        lv_obj_add_event_cb(zero, calib_zero_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 应用：整页唯一的实心主按钮（品牌蓝底白字），视觉权重最高，
     * 让"校准终点"一眼可辨，纯按键操作时也能明确当前该按哪里 */
    lv_obj_t *apply = plain_button(c, "应用校准", LV_PCT(100), 40);
    lv_obj_set_style_bg_color(apply, COL_PRIMARY, 0);
    lv_obj_set_style_border_color(apply, COL_PRIMARY, 0);
    lv_obj_t *apply_lbl = lv_obj_get_child(apply, 0);
    if (apply_lbl != NULL) {
        lv_obj_set_style_text_color(apply_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(apply_lbl, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    }
    lv_obj_add_event_cb(apply, calib_apply_cb, LV_EVENT_CLICKED, NULL);

    /* 焦点链：－ → ＋ → 步长 →（电流通道多一格清零）→ 应用。
     * 顺序即"调标准值 → 选步长 → 应用"的自然操作流，纯按键不绕路。 */
    focus_add(minus);
    focus_add(plus);
    focus_add(step);
    focus_add(zero); /* 电压通道时为 NULL，focus_add 自动忽略 */
    focus_add(apply);
}

/* ============================================================
 * 二级页：显示设置（亮度 + 方向）
 * ============================================================ */

/**
 * @brief 更新角度按钮高亮：永远反映当前**实际显示角度**（自动模式下跟随 IMU）
 *
 * 自动旋转开着时，实际角度会被重力服务改掉，按钮高亮必须以
 * lvgl_port_get_rotation_angle() 的返回值为准，而不是"用户最后点的按钮"。
 */
static void angle_buttons_refresh(void)
{
    uint16_t cur = lvgl_port_get_rotation_angle(); /* 查当前实际生效的显示角度 */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = s_angle_btns[i];
        if (btn == NULL) {
            continue;
        }
        bool sel = (s_angle_vals[i] == cur);
        /* 选中项：品牌蓝底白字；未选中：白底描边墨字 */
        lv_obj_set_style_bg_color(btn, sel ? COL_PRIMARY : lv_color_white(), 0);
        lv_obj_set_style_border_color(btn, sel ? COL_PRIMARY : COL_BORDER, 0);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl != NULL) {
            lv_obj_set_style_text_color(lbl, sel ? lv_color_white() : COL_INK, 0);
        }
    }
}

/**
 * @brief 手动点角度按钮：关自动、持久化、应用该角度
 *
 * 必须走 lvgl_port_set_rotation_angle()（而非直接写面板寄存器）：它在切换
 * 显示方向的同时**同步触摸输入的旋转映射**。若只转显示不转触摸坐标，
 * 会出现"画面转了但手指点的位置对不上"的错位。
 *
 * @param e LVGL 点击事件；user_data 携带目标角度（0/90/180/270）
 */
static void angle_clicked_cb(lv_event_t *e)
{
    uint16_t deg = (uint16_t)(intptr_t)lv_event_get_user_data(e);
    if (lvgl_port_set_rotation_angle(deg) != ESP_OK) {
        return;
    }
    orientation_set_auto(false); /* 手动选角度 = 退出自动旋转模式 */
    /* settings_set_orientation(自动标志, 角度)：经 settings 封装写 NVS（Flash），
     * settings_save_now() 立即落盘（防突然断电丢失） */
    (void)settings_set_orientation(0, deg);
    (void)settings_save_now();
    if (s_auto_switch != NULL && lv_obj_has_state(s_auto_switch, LV_STATE_CHECKED)) {
        /* lv_obj_remove_state：撤销开关的"选中"状态（UI 与逻辑保持一致） */
        lv_obj_remove_state(s_auto_switch, LV_STATE_CHECKED);
    }
    buzzer_play(BUZZER_TONE_CLICK);
    angle_buttons_refresh();
}

/**
 * @brief 自动旋转开关：开 → 重力接管；关 → 保持当前角度并存为手动值
 *
 * 开关不管"转到哪个角度"，只切换"谁说了算"：开了由 orientation 服务按
 * IMU 姿态自动转屏；关了就以当前实际角度为手动值固化下来。
 *
 * @param e LVGL 值变化事件（目标是 lv_switch 开关）
 */
static void auto_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    /* lv_obj_has_state(..., LV_STATE_CHECKED)：查开关当前是否处于"开"状态 */
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    orientation_set_auto(on);
    if (on) {
        /* 开自动：记住自动模式 + 当前角度（下次开机先用这个角度，再交给 IMU） */
        (void)settings_set_orientation(1, lvgl_port_get_rotation_angle());
    } else {
        /* 关自动：把当前角度固化为手动角度 */
        (void)settings_set_orientation(0, lvgl_port_get_rotation_angle());
    }
    (void)settings_save_now(); /* 立即写 NVS，防断电丢失 */
    (void)buzzer_play(BUZZER_TONE_CLICK);
}

/**
 * @brief 亮度滑条变化/松手 → 立即调光 + 更新数值标签 + 落盘
 *
 * 拖动中（VALUE_CHANGED）实时生效并只改内存缓存；松手（RELEASED）才
 * settings_save_now() 写 Flash——避免拖动过程高频写 NVS（Flash 有擦写寿命）。
 *
 * @param e LVGL 事件（VALUE_CHANGED=拖动中 / RELEASED=松手，同一个回调两种触发）
 */
static void bright_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    /* lv_slider_get_value()：读滑动条当前值（范围由 lv_slider_set_range 事先限定） */
    uint8_t val = (uint8_t)lv_slider_get_value(slider);

    /* 立即生效 + 持久化（防抖由 settings 内部处理）：
     * lcd_nv3030b_set_backlight() 调背光 PWM 立即见光；
     * settings_set_lcd_brightness() 只更新内存缓存并标"脏"，延时防抖写 Flash */
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

/**
 * @brief 打开"显示设置"二级页（亮度 + 自动旋转 + 角度按钮）
 *
 * 初始值从 settings（内存缓存，读取不碰 Flash）取；取不到用安全默认值。
 */
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

    lv_obj_t *slider = lv_slider_create(c); /* lv_slider_create：LVGL 滑动条部件 */
    /* 内缩到 84%：旋钮在两端时仍离面板边缘约 30px，拇指不会按不到；
       也避开圆角侵入区（两端各多留 8px 内边距）。
       这就是项目约定"滑条预留可触摸区域"的落点。 */
    lv_obj_set_width(slider, LV_PCT(84));
    lv_slider_set_range(slider, BRIGHT_MIN, BRIGHT_MAX); /* LVGL 滑动条：设最小/最大值 */
    lv_slider_set_value(slider, bright, LV_ANIM_OFF);    /* 设初值，LV_ANIM_OFF=不做动画直接就位 */
    lv_obj_set_height(slider, 24); /* 加大主体高度，触摸更好点中 */
    /* LV_PART_KNOB：滑动条的"旋钮"子部件，可单独设样式 */
    lv_obj_set_style_width(slider, 20, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 26, LV_PART_KNOB);
    /* 同一回调挂两个事件：拖动中实时生效（VALUE_CHANGED）、松手落盘（RELEASED） */
    lv_obj_add_event_cb(slider, bright_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, bright_slider_cb, LV_EVENT_RELEASED, NULL);
    focus_add(slider); /* 焦点链第 1 项：亮度滑条（长按 K2 进调节模式） */

    /* ---- 屏幕方向：自动开关 + 角度按钮（0°=默认横屏，顺时针）---- */
    lv_obj_t *auto_row = plain_container(c);
    lv_obj_set_size(auto_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(auto_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(auto_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(auto_row, 8, 0);
    plain_label(auto_row, "自动旋转（随握持方向）", LVGL_PORT_FONT_16, COL_INK, false);
    s_auto_switch = lv_switch_create(auto_row); /* lv_switch_create：LVGL 开关部件（两态：开/关） */
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
    /* lv_obj_add_state(..., LV_STATE_CHECKED)：把开关置为"开"态（按持久化配置恢复） */
    if (settings_get(&cfg) == ESP_OK && cfg.orientation_auto) {
        lv_obj_add_state(s_auto_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_auto_switch, auto_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    focus_add(s_auto_switch); /* 焦点链第 2 项：自动旋转开关（长按 K2 切换） */

    lv_obj_t *cap2 = plain_label(c, "屏幕方向", LVGL_PORT_FONT_16, COL_INK, false);
    lv_obj_set_width(cap2, LV_PCT(100));
    lv_obj_set_style_margin_top(cap2, 6, 0);

    lv_obj_t *rot_row = plain_container(c);
    lv_obj_set_size(rot_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rot_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    static const char *angle_names[4] = { "0°", "90°", "180°", "270°" };
    /* 四个角度按钮：角度值经 user_data 传给共用回调 angle_clicked_cb */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = plain_button(rot_row, angle_names[i], 58, 36);
        lv_obj_add_event_cb(b, angle_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_angle_vals[i]);
        s_angle_btns[i] = b; /* 记下按钮指针，供 angle_buttons_refresh 改高亮 */
        focus_add(b);        /* 焦点链第 3~6 项：0/90/180/270° 角度按钮 */
    }
    angle_buttons_refresh(); /* 打开页时按实际角度点亮对应按钮 */

    lv_obj_t *note = plain_label(c, "0° 为默认横屏，顺时针旋转；自动模式下角度高亮跟随设备姿态。",
                                 LVGL_PORT_FONT_12, COL_GRAY, false);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

/* ============================================================
 * 二级页：蜂鸣器响度
 * ============================================================ */

/**
 * @brief 响度滑条变化/松手 → 立即改音量 + 更新数值标签 + 落盘
 *
 * 与亮度滑条同款策略：拖动中只改内存缓存（buzzer_set_volume 立即生效），
 * 松手才 settings_save_now() 写 Flash（省 NVS 擦写寿命）。
 *
 * @param e LVGL 事件（VALUE_CHANGED=拖动中 / RELEASED=松手）
 */
static void volume_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t val = (uint8_t)lv_slider_get_value(slider);

    /* buzzer_set_volume：调蜂鸣器驱动音量立即生效；settings_set_buzzer_volume：持久化 */
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

/**
 * @brief "试听"按钮：按当前音量播一段提示音（调完滑条立刻能验听感）
 *
 * @param e LVGL 点击事件（未使用）
 */
static void buzzer_test_cb(lv_event_t *e)
{
    (void)e;
    (void)buzzer_play(BUZZER_TONE_OK); /* 播放"成功"音型作为试听样本 */
}

/**
 * @brief 打开"蜂鸣器响度"二级页
 *
 * 音量初值优先取驱动当前值（buzzer_get_volume），其次取持久化配置，
 * 都取不到用安全默认 40%。
 */
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
        vol = drv_vol; /* 驱动值更新，以它为准 */
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

    lv_obj_t *slider = lv_slider_create(c); /* LVGL 滑动条 */
    lv_obj_set_width(slider, LV_PCT(84)); /* 与亮度滑条一致内缩，两端旋钮可触摸 */
    lv_slider_set_range(slider, VOLUME_MIN, VOLUME_MAX); /* 限定合法音量范围 */
    lv_slider_set_value(slider, vol, LV_ANIM_OFF);
    lv_obj_set_height(slider, 24); /* 加大主体高度，触摸更好点中 */
    lv_obj_set_style_width(slider, 20, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 26, LV_PART_KNOB);
    /* 拖动实时生效 / 松手落盘（同 bright_slider_cb 的双事件策略） */
    lv_obj_add_event_cb(slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, volume_slider_cb, LV_EVENT_RELEASED, NULL);
    focus_add(slider); /* 焦点链第 1 项：响度滑条（长按 K2 进调节模式） */

    /* "试听"按钮：调完滑条立刻播放提示音，让用户当场确认响度是否合适，
     * 而不是退出设置页后才发现音量没调对 */
    lv_obj_t *test = plain_button(c, "试听", LV_PCT(60), 40);
    lv_obj_add_event_cb(test, buzzer_test_cb, LV_EVENT_CLICKED, NULL);
    focus_add(test); /* 焦点链第 2 项：试听按钮 */

    lv_obj_t *note = plain_label(c, "提示音用于按键反馈与启动完成提示；\n调到 0 以上才有声音。",
                                 LVGL_PORT_FONT_12, COL_GRAY, false);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

/* ============================================================
 * 二级页：WiFi（路线图说明，联网能力在 v0.5.0）
 * ============================================================ */

/**
 * @brief 打开"WiFi 与热点"二级页（纯说明页，无任何可操作控件）
 *
 * v0.5.0 才有联网能力，本页只做"路线图"诚实告知，不放假开关钓用户预期。
 * 全文都是静态文本，因此页面里没有按钮——这不影响"纯按键可达"约定：
 * 用户随时可按标题栏"返回"离开。
 */
static void wifi_open_cb(void)
{
    lv_obj_t *c = subpage_open(SUB_WIFI, "WiFi 与热点");

    /* LV_SYMBOL_WIFI：LVGL 内置 WiFi 字形符号（免图片资源） */
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
    /* lv_label_set_long_mode(..., LV_LABEL_LONG_WRAP)：超宽自动折行（不截断） */
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

/**
 * @brief 电能清零"确定"动作：执行清零 + 提示音 + 结果提示
 *
 * 由 show_confirm 的 ok_cb 调用（确认框已先被关闭）。
 * 清零由 power_measure_reset_energy 在测量核块边界执行（约 0.2s 后生效），
 * 不立即自动关本页：让用户留在电能页亲眼看到数值归零。
 */
static void energy_reset_ok(void)
{
    (void)power_measure_reset_energy(); /* 清零累计电能与运行时间（防跨核撕裂） */
    buzzer_play(BUZZER_TONE_OK);
    show_notice("已清零", "电能与运行时间已重新开始统计。");
}

/**
 * @brief "清零统计"按钮：破坏性操作先弹确认框（不可撤销必须二次确认）
 *
 * @param e LVGL 点击事件（未使用）
 */
static void energy_reset_clicked_cb(lv_event_t *e)
{
    (void)e;
    /* show_confirm：取消/确定双按钮确认框，确定后才走 energy_reset_ok */
    show_confirm("清零统计？", "将把“累计电能”与“运行时间”归零，\n此操作不可撤销。", energy_reset_ok);
}

/**
 * @brief 打开"电能统计"二级页（累计电能/运行时间 + 清零）
 *
 * 数值初值取自 s_last_snap（5Hz 快照缓存），打开后由
 * screen_settings_refresh() 持续刷新（见文件头"实时数值刷新"）。
 */
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
    /* 危险操作按钮用红色边框+红字提示风险（与"确定"类主按钮视觉区分） */
    lv_obj_set_style_border_color(btn, COL_DANGER, 0);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl != NULL) {
        lv_obj_set_style_text_color(lbl, COL_DANGER, 0);
        lv_obj_set_style_text_font(lbl, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    }
    lv_obj_add_event_cb(btn, energy_reset_clicked_cb, LV_EVENT_CLICKED, NULL);
    focus_add(btn); /* 焦点链第 1 项：清零统计（长按 K2 弹确认框） */

    /* 单行短提示（原长句会折行把本页撑到需滚动；按钮降到 40px + 短句
     * 后整页一屏放下，底部不贴边）。
     * "0.2 秒生效"是实测的清零延迟：power_measure_reset_energy 只是发起请求，
     * 测量核在下一个块边界才真正归零，提前提醒用户别以为清零没生效。 */
    lv_obj_t *note = plain_label(c, "清零约 0.2 秒后生效", LVGL_PORT_FONT_12,
                                 COL_GRAY, false);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
}

/* ============================================================
 * 二级页：关于本机（对话框）
 * ============================================================ */

/**
 * @brief 打开"关于本机"信息框（对话框形式，无二级页）
 *
 * 固件版本号经 esp_app_get_description()->version 取自编译期写入的应用描述
 * （ESP-IDF 机制：idf.py 烧录时把版本字符串嵌进固件镜像，无需手工维护）。
 */
static void about_open_cb(void)
{
    char text[112];
    snprintf(text, sizeof(text), "XT60 功率计\n固件版本 v%s\n作者博客：blog.zeruns.com",
             esp_app_get_description()->version); /* ESP-IDF：读应用描述（含版本号） */
    show_notice("关于本机", text);
}

/* ============================================================
 * 首页网格
 * ============================================================ */

typedef void (*cell_cb_t)(void); /**< 网格单元的打开动作（无参无返回） */

/**
 * @brief 网格点击的中间回调（LVGL 回调签名适配）
 *
 * LVGL 事件回调签名是 void(lv_event_t*)，与业务函数签名不同；用 user_data
 * 把业务函数指针传进来，由本回调转发，六个格子共用一个 LVGL 回调。
 *
 * @param e LVGL 点击事件；user_data 携带 cell_cb_t 业务函数
 */
static void cell_clicked_cb(lv_event_t *e)
{
    cell_cb_t cb = (cell_cb_t)lv_event_get_user_data(e);
    if (cb != NULL) {
        cb();
    }
}

/**
 * @brief 创建一个网格单元（图标 + 名称，点击进入 cb）
 *
 * 单元格宽随行 flex_grow 均分，高固定 CELL_HEIGHT(48px)——触摸目标标准。
 *
 * @param row   行容器（一行两格）
 * @param icon  LVGL 内置符号（LV_SYMBOL_*）
 * @param name  功能名称文本
 * @param cb    点击后打开对应二级页的函数
 * @return 创建好的单元格按钮
 */
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

/**
 * @brief 在设置 tile 上构建设置首页（2×3 功能网格）
 *
 * 由 ui_nav 初始化时调用一次；二级页浮层以 tile 为父级，随 tile 一起被
 * 横向滑动（不跨页）。
 *
 * @param tile tileview 的设置页对象（本页所有内容的根父级）
 */
void screen_settings_create(lv_obj_t *tile)
{
    s_tile = tile;
    /* 旋转重建：旧浮层已随 tileview 被 lv_obj_delete 连带删除，这里只能置空
     * 指针（不能再调 subpage_close，它会二次删除已释放对象）；但定时器与
     * 焦点链是独立于对象树的悬空状态，必须手动作废。 */
    if (s_close_timer != NULL) {
        lv_timer_delete(s_close_timer);
        s_close_timer = NULL;
    }
    focus_end();
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
    /* 网格一屏尽收：高度精确等于 3 行 + 2 行距，不给滚动留任何余地 */
    lv_obj_set_size(grid, LV_PCT(100), 3 * CELL_HEIGHT + 2 * CELL_GAP_Y);
    lv_obj_set_pos(grid, 0, GRID_TOP_Y);
    lv_obj_set_style_pad_left(grid, GRID_SIDE_PAD, 0);
    lv_obj_set_style_pad_right(grid, GRID_SIDE_PAD, 0);
    lv_obj_set_style_pad_row(grid, CELL_GAP_Y, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(grid, false);

    /* 六格功能表：图标 + 名称 + 打开函数，按行序 r*2 / r*2+1 两两成行。
     * 2×3 网格恰好 6 格；替换某格功能只改这一行，排版代码不用动。 */
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
        s_grid_cells[r * 2] =
            make_cell_dispatch(row, cells[r * 2].icon, cells[r * 2].name, cells[r * 2].cb);
        s_grid_cells[r * 2 + 1] =
            make_cell_dispatch(row, cells[r * 2 + 1].icon, cells[r * 2 + 1].name,
                               cells[r * 2 + 1].cb);
    }
    /* 重建（旋转/首次）后网格焦点模式归零：按钮是新对象，旧高亮状态不可带过来 */
    s_grid_focus_on = false;
    s_grid_idx = 0;
}

/**
 * @brief 数据刷新（ui_nav_refresh 以 5Hz 转发快照，见 screen_settings.h）
 *
 * 无论当前打开哪个二级页，都先把快照存进 s_last_snap（校准向导应用增益
 * 时要用"当前读数"，即使此刻不在校准页也保持最新）。随后按 s_sub_kind
 * 分发：只刷新"打开中"的页面对应的标签，未打开的跳过（省无谓的字符串
 * 格式化与 LVGL 重绘）。
 *
 * @param snap 最新测量快照，不可为 NULL
 */
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
        /* 校准向导：当前读数以 5Hz 刷新，用户对着标准表实时观察偏差 */
        if (s_cal_read_label != NULL) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%.5f %s",
                     (double)(s_cal_is_voltage ? snap->voltage_v : snap->current_a),
                     s_cal_is_voltage ? "V" : "A");
            lv_label_set_text(s_cal_read_label, buf);
        }
        break;
    case SUB_ENERGY:
        /* 电能页：清零后用户留在本页，靠这里的 5Hz 刷新亲眼看到数值归零 */
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

/**
 * @brief 弹窗按键动作：K1=取消 / K2=确定（固定键位，无需焦点记忆）
 *
 * footer 按钮即 msgbox 底部按钮区的子对象（"取消"在前、"确定"在后，
 * 见 show_confirm / show_notice 的创建顺序）。激活方式与 focus_activate
 * 同款：合成一次 LV_EVENT_CLICKED，注册回调（含 user_data）照常执行，
 * 与手指点击完全等价。确定动作可能再弹结果提示框（如"已清零"），
 * s_active_msgbox 会随之更新，调用方拿它当"当前弹窗"用是安全的。
 *
 * @param id K1=BUTTON_ID_BOOT → 取消；K2=BUTTON_ID_USER → 确定
 */
static void msgbox_key_action(button_id_t id)
{
    lv_obj_t *box = s_active_msgbox;
    if (box == NULL) {
        return;
    }
    lv_obj_t *footer = lv_msgbox_get_footer(box); /* 底部按钮区（可能为 NULL） */
    uint32_t n = (footer != NULL) ? lv_obj_get_child_count(footer) : 0;
    if (n == 0) {
        /* 无按钮的异常框：任一键直接关闭，绝不把用户困在弹窗里 */
        lv_msgbox_close(box);
        return;
    }
    if (id == BUTTON_ID_USER) {
        /* K2 = 确定 = footer 最后一个按钮 */
        lv_obj_send_event(lv_obj_get_child(footer, n - 1), LV_EVENT_CLICKED, NULL);
    } else {
        /* K1 = 取消 = footer 第一个按钮（单按钮提示框时它就是"确定"，
         * 点了同样关闭——提示框只求"看完能走"，两个键都认） */
        lv_obj_send_event(lv_obj_get_child(footer, 0), LV_EVENT_CLICKED, NULL);
    }
}

/**
 * @brief 查询设置页是否有模态浮层（二级页/弹窗）正在打开——不管当前在哪一页
 *
 * 供 ui_nav 的滑动识别使用：模态浮层打开时必须抑制"环形翻页手势"，否则
 * 一次横滑就把承载浮层的设置 tile 整个翻走，浮层还留在原 tile 上（视觉上
 * 像"页面被抽走、菜单悬空"，还会连带触发按键路由错乱，见 subpage_is_open）。
 */
bool screen_settings_modal_is_open(void)
{
    return (s_subpage != NULL || s_active_msgbox != NULL);
}

/**
 * @brief 查询设置页是否需要接管按键（ui_nav 按键路由的门，见 screen_settings.h）
 */
bool screen_settings_subpage_is_open(void)
{
    /* 只有"模态浮层存在 **且** 当前就在设置页"才算本页要接管按键。
     * 【关键】二级页浮层是挂在设置 tile 上的，tileview 被环形翻页手势翻走后
     * 浮层仍留在设置 tile 上（对象还活着），若不加页面限定，会在测量/波形页
     * 也判成"模态中"，把全局按键全劫持——表现为无法翻页/无法开菜单、页面
     * 在两页间反复震荡（2026-10 模拟器实证的根因）。 */
    if ((s_subpage != NULL || s_active_msgbox != NULL) &&
        ui_nav_current_page() == UI_PAGE_SETTINGS) {
        return true; /* 二级页/弹窗且在设置页：模态，按键全归本页 */
    }
    /* 设置页首页也返回 true：网格入口（长按 K2 进入网格焦点模式）必须能
     * 收到按键才可达，而 ui_nav 的路由门是"本接口 true 才调 screen_settings_key"。
     * 未进入网格模式时 screen_settings_key 一律返回 false，事件会沿 ui_nav 的
     * else-if 链落回三页导航（单击=翻页、长按 K1=回测量页），行为与从前一致。 */
    return ui_nav_current_page() == UI_PAGE_SETTINGS;
}

/**
 * @brief 设置页纯按键操作总入口（焦点链 + 网格 + 弹窗三层，见 screen_settings.h 契约）
 *
 * 处理优先级：弹窗（模态）＞ 二级页焦点链 ＞ 首页网格。
 * 返回 false 表示本页不消费该事件（首页未进网格模式时的普通按键），
 * ui_nav 会继续走测量/波形菜单与三页导航——这正是"翻页键位不受影响"的保证。
 */
bool screen_settings_key(button_id_t id, button_event_t ev)
{
    /* ---- ① 弹窗层：K1=取消 / K2=确定，其余事件一律吞掉 ---- */
    if (s_active_msgbox != NULL) {
        if (ev == BUTTON_EVENT_SINGLE_CLICK || ev == BUTTON_EVENT_LONG_PRESS_START) {
            msgbox_key_action(id);
        }
        /* 长按松开/双击等无动作但不外漏（防止长按结束的 RELEASE 落到翻页逻辑） */
        return true;
    }

    /* ---- ② 二级页焦点链 ---- */
    if (s_subpage != NULL) {
        switch (ev) {
        case BUTTON_EVENT_SINGLE_CLICK:
            if (s_slider_edit) {
                /* 滑条调节模式：K1=减、K2=加（与"K1=上一项/K2=下一项"同向心智） */
                slider_step(id == BUTTON_ID_BOOT ? -1 : +1);
            } else {
                focus_move(id == BUTTON_ID_BOOT ? -1 : +1);
            }
            return true;
        case BUTTON_EVENT_LONG_PRESS_START:
            if (id == BUTTON_ID_USER) {
                if (s_slider_edit) {
                    slider_edit_exit(); /* 长按 K2 = 退出调节模式（补发 RELEASED 落盘） */
                } else {
                    focus_activate(); /* 长按 K2 = 激活焦点控件 */
                }
            } else {
                /* 长按 K1 = 返回/关闭本子页（等效触屏"返回"按钮） */
                if (s_slider_edit) {
                    slider_edit_exit(); /* 先落盘再走，防止调节中的值丢失 */
                }
                if (s_focus_back != NULL) {
                    /* 合成点击"返回"：走 back_clicked_cb → subpage_close()，
                     * 与触屏路径同一出口（含定时器删除/焦点链清理） */
                    lv_obj_send_event(s_focus_back, LV_EVENT_CLICKED, NULL);
                } else {
                    subpage_close();
                }
            }
            return true;
        default:
            /* 双击/长按松开：子页开着时全吞，防止外漏成翻页/开菜单 */
            return true;
        }
    }

    /* ---- ③ 设置首页网格 ---- */
    if (ui_nav_current_page() != UI_PAGE_SETTINGS) {
        return false; /* 不在设置页（异常场景兜底）：不碰按键 */
    }
    if (!s_grid_focus_on) {
        /* 未进网格模式：仅长按 K2 进入（与"长按 K2=开本页菜单"同心智）；
         * 其余按键交回 ui_nav 翻页（返回 false 走 else-if 链） */
        if (ev == BUTTON_EVENT_LONG_PRESS_START && id == BUTTON_ID_USER) {
            s_grid_focus_on = true;
            s_grid_idx = 0;
            grid_paint(); /* 高亮第一格，用户一眼看到"焦点在哪" */
            return true;
        }
        return false;
    }
    switch (ev) {
    case BUTTON_EVENT_SINGLE_CLICK:
        /* 焦点上下移动（环形）：K1=上一格、K2=下一格 */
        s_grid_idx = (s_grid_idx + (id == BUTTON_ID_BOOT ? -1 : 1) + GRID_CELL_COUNT) %
                     GRID_CELL_COUNT;
        grid_paint();
        return true;
    case BUTTON_EVENT_LONG_PRESS_START:
        if (id == BUTTON_ID_USER) {
            /* 打开焦点功能格：合成 CLICKED 复用格子的打开回调（cell_clicked_cb），
             * 子页/弹窗随即接管按键（上面的 ①② 层） */
            lv_obj_send_event(s_grid_cells[s_grid_idx], LV_EVENT_CLICKED, NULL);
            return true;
        }
        /* 长按 K1 = 退出网格模式；返回 false 让 ui_nav 继续"设置页长按 K1=回测量页"，
         * 即"退出焦点 + 返回"一步完成（与子页内长按 K1=返回 同语义） */
        s_grid_focus_on = false;
        grid_paint();
        return false;
    default:
        return false;
    }
}