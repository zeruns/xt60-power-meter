/**
 * @file screen_settings.c
 * @brief 设置页：2×4 功能网格入口 + 六个功能二级页（v0.2.0 设置全可用）
 *
 * ────────────────────────────────────────────────────────────
 * 版式（284×240；顶部 22px 状态栏由 ui_nav 共享，不在本页职责内）
 * ────────────────────────────────────────────────────────────
 * 首页（网格一屏尽收，无需滚动）：
 *   ┌ x=14 ──────────────────── x=270 ┐
 *   │ 设置                              │ y≈28 标题（Bold 20）
 *   │ ┌──────────┐ ┌──────────┐        │ y=52
 *   │ │ 测量设置  │ │ 显示设置  │        │ 每格约 122×40
 *   │ └──────────┘ └──────────┘        │
 *   │ ┌──────────┐ ┌──────────┐        │ y=98
 *   │ │ 蜂鸣器    │ │ WiFi     │        │
 *   │ └──────────┘ └──────────┘        │
 *   │ ┌──────────┐ ┌──────────┐        │ y=144
 *   │ │ 电能统计  │ │ 告警设置  │        │
 *   │ └──────────┘ └──────────┘        │
 *   │ ┌──────────┐ ┌──────────┐        │ y=190
 *   │ │ 关于本机  │ │ 恢复出厂  │        │ 第 8 格＝恢复出厂（F6）
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
 *   · 告警阈值：settings_get/set_alarm（v_en/i_en/p_en 使能 + V/A/W 三阈值，
 *     判定在测量核做，本页只管持久化用户设定）；
 *   · WiFi：wifi_manager（状态查询/忘记网络，F7 真实状态页）；
 *   · 恢复出厂：settings_restore_factory + power_measure_reset_energy +
 *     wifi_manager_forget 三步编排（标定保留，F6）。
 *
 * ────────────────────────────────────────────────────────────
 * 页面结构（各子页一览）
 * ────────────────────────────────────────────────────────────
 *   首页 2×4 网格 → 点击进入对应二级页：
 *   · 测量设置  → 校准向导（通道选择 → 标准值输入 → 完成提示，三级）
 *   · 显示设置  → 亮度滑条 + 自动旋转开关 + 自动息屏档位（F4）+ 角度按钮
 *   · 蜂鸣器    → 响度滑条 + 试听按钮
 *   · WiFi      → 状态页（状态/IP 或热点名 + 忘记网络，F7）
 *   · 电能统计  → 累计电能/运行时间 + 确认框清零
 *   · 告警设置  → 过压/过流/过功率三阈值（＋/－ 步进）+ 三项使能开关
 *   · 关于本机  → 信息对话框（无二级页）
 *   · 恢复出厂  → 确认框 → 清用户设置/电能统计/WiFi（标定保留，F6）
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
#include "esp_log.h" /* ESP_LOGD：二级页进出日志（卡死定位用，默认 DEBUG 级不输出） */
#include "esp_timer.h"
#include "lvgl.h"
#include "lvgl_port.h"

/* FreeRTOS：忘记网络的一次性任务需要创建/自删任务（见 wifi_forget_task）。
 * xTaskCreatePinnedToCore / vTaskDelete 由 task.h 提供；
 * FreeRTOS.h 必须先包含（task.h 依赖它的类型定义）。 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "button.h"
#include "buzzer.h"
#include "data_store.h"
#include "lcd_nv3030b.h"
#include "orientation.h"
#include "power_measure.h"
#include "settings.h"
#include "wifi_manager.h" /* F7：WiFi 状态页（wifi_manager_get_state/get_ip/get_ap_ssid/forget） */
/* ui_nav：取当前页码（ui_nav_current_page）——设置首页的网格焦点模式需要
 * 知道"用户此刻真的在设置页"，否则按键接管会波及测量/波形页的翻页键位 */
#include "ui_nav.h"

/** ESP_LOG 日志标签（过滤本模块日志用，如 `idf.py monitor` 里按 set_ui 过滤） */
static const char *TAG = "set_ui";

/* ---- 色板（与测量页/波形页同一语义色） ----
 * lv_color_hex(0xRRGGBB)：把网页风格的十六进制色值转换成 LVGL 内部的
 * lv_color_t 颜色结构，之后所有 set_style_*_color 接口都收这种颜色。 */
#define COL_PRIMARY  lv_color_hex(0x0066CC)
#define COL_INK      lv_color_hex(0x1A1A1A)
#define COL_GRAY     lv_color_hex(0x6B7280)
#define COL_BORDER   lv_color_hex(0xB4BEC9) /* 控件边框：边界清晰（原 #E3E7EC 过浅） */
#define COL_CARD_BG  lv_color_hex(0xF1F4F8) /* 卡片底：与白底拉开可辨差异 */
#define COL_PRESSED  lv_color_hex(0xCFE0F7) /* 按下底色：与白底一眼可辨（Bug2 按压反馈修复） */
#define COL_OK       lv_color_hex(0x1E9E4F)
#define COL_DANGER   lv_color_hex(0xD0392B)

/* ---- 首页网格几何 ---- */
#define GRID_SIDE_PAD 14 /**< 左右内缩（避开面板圆角） */
#define GRID_TOP_Y    52 /**< 网格起始 y */
#define CELL_HEIGHT   40 /**< 单格高度（40px 仍达触摸目标下限；4 行后压到此高度
                          *   才能一屏放下 7 个功能格） */
#define CELL_GAP_X    10 /**< 网格两列之间的间距 */
#define CELL_GAP_Y    8  /**< 网格四行之间的间距 */

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
    SUB_WIFI,       /**< WiFi 状态与配网（F7 真实状态页） */
    SUB_WIFI_SCAN,  /**< WiFi 扫描结果（独立页，v0.6.1 需求 3） */
    SUB_ENERGY,     /**< 电能统计与清零 */
    SUB_ALARM,      /**< 告警设置：过压/过流/过功率阈值 + 使能 */
} sub_kind_t;

static lv_obj_t *s_tile = NULL;      /**< 设置页 tile（二级页浮层的父级） */
static lv_obj_t *s_subpage = NULL;   /**< 二级页浮层；NULL = 未打开 */
static lv_obj_t *s_sub_content = NULL; /**< 二级页内容容器 */
static sub_kind_t s_sub_kind = SUB_NONE; /**< 当前二级页类型（刷新分发的依据） */
static bool s_layer_hooked = false;  /**< 对话框样式与关闭处理是否已安装 */
static lv_obj_t *s_msgbox_scrim = NULL; /**< 当前对话框的压暗遮罩（随框删除） */
static lv_obj_t *s_active_msgbox = NULL; /**< 当前打开的对话框（NULL=无；按键路由据此判模态） */

static void close_active_msgbox(void);   /* 前向声明（定义见下方；弹新框前关旧框，防叠窗） */
static void passkey_close(void);         /* 前向声明：WiFi 密码浮层（v0.6.0），见下方 WiFi 一节 */
static void numkey_close(void);          /* 前向声明：数字键盘浮层（v0.6.0），见下方告警一节 */

static data_store_snapshot_t s_last_snap; /**< 最近快照（校准向导/电能统计使用） */

/* 刷新用标签（所在二级页关闭时置 NULL） */
static lv_obj_t *s_cal_read_label = NULL;     /**< 校准向导：当前读数 */
static lv_obj_t *s_energy_wh_label = NULL;    /**< 电能统计：累计电能 */
static lv_obj_t *s_energy_rt_label = NULL;    /**< 电能统计：运行时间 */
static lv_obj_t *s_bright_value_label = NULL; /**< 显示设置：亮度数值 */
static lv_obj_t *s_vol_value_label = NULL;    /**< 蜂鸣器：响度数值 */
static lv_obj_t *s_wifi_state_label = NULL;   /**< WiFi 页：状态行（5Hz 刷新，F7） */
static lv_obj_t *s_wifi_hint_label = NULL;    /**< WiFi 页：配网提示行（随状态切换） */

/* ---- WiFi 页 v0.6.0 改版的状态（放在此处而非 WiFi 一节：subpage_close
 *      需要清理它们，而 subpage_close 定义在文件靠前的位置） ---- */

/** 三档模式按钮（下标 = wifi_manager_mode_t：0=自动 1=仅 STA 2=热点） */
static lv_obj_t *s_wifi_mode_btns[3] = {0};
static lv_obj_t *s_wifi_list = NULL;      /**< 扫描结果列表容器（行动态创建） */
static lv_obj_t *s_wifi_scan_btn = NULL;  /**< "搜索 Wi-Fi"按钮（扫描中置灰） */
static lv_timer_t *s_wifi_scan_timer = NULL; /**< 扫描轮询定时器（300ms 一次） */

/** 最近一次扫描结果的一份拷贝（仅 UI 任务访问）：点击某一行时取它的 SSID */
#define WIFI_UI_LIST_MAX 12 /**< 列表最多渲染几个热点（对象数量与可滚长度的折中） */
static wifi_ap_record_t s_wifi_ap_copy[WIFI_UI_LIST_MAX];
static uint16_t s_wifi_ap_copy_n = 0;

/** 密码输入浮层（全屏浮层 + LVGL 文字键盘；触屏输入） */
static lv_obj_t *s_passkey_page = NULL; /**< 浮层根对象；NULL=未打开 */
static lv_obj_t *s_passkey_ta = NULL;   /**< 浮层内的密码输入框 */
static char s_passkey_ssid[WIFI_MANAGER_SSID_MAX_LEN + 1]; /**< 待连接的 SSID */

/** 一次性的"连接路由器"任务与结果传递（与"忘记网络"同款异步模式） */
static bool s_wifi_conn_running = false;      /**< 已有一次连接在跑（防重复提交） */
static esp_err_t s_wifi_conn_result = ESP_OK; /**< wifi_manager_connect 的返回值 */
static char s_conn_ssid[WIFI_MANAGER_SSID_MAX_LEN + 1];
static char s_conn_pass[WIFI_MANAGER_PASS_MAX_LEN];
static lv_obj_t *s_alarm_value_labels[3] = { 0 }; /**< 告警设置：三阈值数值标签（V/A/W） */

/** 告警设置页：当前编辑中的配置（改完即 settings_set_alarm，防抖落盘） */
static settings_alarm_t s_alarm_cfg;

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

#define GRID_CELL_COUNT 8 /**< 设置首页网格功能格数（7 个功能 + 1 个留空位） */

/* ---- 设置首页网格焦点（二级页全关时由长按 K2 进入） ---- */
static lv_obj_t *s_grid_cells[GRID_CELL_COUNT] = { 0 }; /**< 各格按钮（登记顺序＝焦点顺序） */
static bool s_grid_focus_on = false; /**< true = 网格焦点模式（单击移焦点/长按 K2 打开焦点格） */
static int s_grid_idx = 0;           /**< 网格焦点格下标（跨子页往返保持，返回后接着来） */

/** 校准完成 1.4s 自动关页定时器（句柄管理：关页/切页时显式删除，防误关新页面） */
static lv_timer_t *s_close_timer = NULL;
static lv_timer_t *s_zero_timer = NULL; /**< 清零采样定时器（40ms 轮询；声明在此因 subpage_close 等需提前引用） */

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
    /* 叠窗防御（一键清零卡死修复）：已有活动弹窗先同步关闭，保证任意时刻
     * 至多一个弹窗——否则单例 scrim/active 指针被新框覆盖，旧框成孤儿。 */
    close_active_msgbox();

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
 * @brief 关闭当前活动弹窗（无则空操作）
 *
 * 同步关闭：box_deleted_cb 随之清理压暗遮罩与 s_active_msgbox。
 * 【一键清零卡死修复 2026-10】清零流程先弹"正在清零"，1 秒后再弹"已清零"
 * ——两框叠放时 s_active_msgbox/s_msgbox_scrim（单例指针）被新框覆盖，
 * 旧框与其遮罩成孤儿永远关不掉（点确定后仍压暗、界面卡死）。
 * 弹新框前先经本函数关旧框，叠窗问题从入口根治。
 */
static void close_active_msgbox(void)
{
    if (s_active_msgbox != NULL) {
        lv_msgbox_close(s_active_msgbox); /* 同步删除，回调自动清 scrim/句柄 */
    }
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
 * @brief 触屏按下反馈样式（Bug2 修复：按下动效不可感知）
 *
 * 根因（2026-10 排查 + git 对比）：按下态只把底色从 #FFFFFF 换成 #E8F0FE
 * （亮度差约 4%），肉眼几乎不可感知；物理按键焦点框（2px 蓝 outline）引入后，
 * 触屏"按下没反馈"的感知落差被放大成"隔离感"。样式本身从未变过——是反馈
 * 强度长期不足，被按键体系的强反馈衬托出来。
 * 修复：按下态加深底色、描边换主色，并加 2px 主色轮廓——与按键焦点框同一
 * 视觉语言（"可操作反馈 = 蓝框"），按下瞬间获得清晰、一致的确认感。
 *
 * @param btn 目标按钮（lv_button）
 */
static void press_feedback_styles(lv_obj_t *btn)
{
    /* LV_STATE_PRESSED 是"状态选择器"：同一属性可按状态分别设值——
     * 第 4 参传 0 是默认态，传 LV_STATE_PRESSED 是"被按下时"才生效的样式 */
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_color(btn, COL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0); /* COVER=完全不透明（默认透明会导致看不到底色） */
    lv_obj_set_style_border_color(btn, COL_BORDER, 0);
    lv_obj_set_style_border_color(btn, COL_PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0); /* 1px 边框让按钮在白底上可辨 */
    lv_obj_set_style_outline_width(btn, 2, LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(btn, COL_PRIMARY, LV_STATE_PRESSED);
    lv_obj_set_style_outline_pad(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 8, 0);       /* 8px 圆角 */
    lv_obj_set_style_shadow_width(btn, 0, 0); /* 去掉主题自带阴影，保持扁平风格 */
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
    press_feedback_styles(btn);

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

#define FOCUS_MAX 10  /**< 焦点链容量（最长子页 9 控件：告警页 3×2± + 3 开关） */
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
 * @brief 网格焦点环形移动（跳过留空格，见 make_cell_dispatch 的 cb=NULL 分支）
 * @param dir +1=下一格 / -1=上一格（负数取模先加长度防溢出）
 */
static void grid_focus_move(int dir)
{
    for (int n = 0; n < GRID_CELL_COUNT; n++) {
        s_grid_idx = (s_grid_idx + dir + GRID_CELL_COUNT) % GRID_CELL_COUNT;
        if (s_grid_cells[s_grid_idx] != NULL) {
            break; /* 落在功能格上即停（留空格自动略过） */
        }
    }
    grid_paint();
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
    /* 【v0.6.0 卡死定位】离页日志（与 subpage_open 的"进入"日志成对） */
    ESP_LOGD(TAG, "离开二级页：%s", screen_settings_current_subpage_name());
    /* 自动关页定时器：页都关了必须删掉，否则 1.4s 后回调会去关一个
     * 不存在（或已被新页面占用）的浮层（任务 2 的根因之一）。 */
    if (s_close_timer != NULL) {
        lv_timer_delete(s_close_timer); /* lv_timer_delete：从 LVGL 定时器链表摘除并释放 */
        s_close_timer = NULL;           /* 置 NULL 防悬空：删完的句柄绝不能再 delete */
    }
    /* 【B12 修复】清零采样定时器同样必须随页失效：否则 1 秒窗口结束后回调会在
     * 已关闭的页面上弹"已清零"浮层（与 s_close_timer 同型悬空 bug）。 */
    if (s_zero_timer != NULL) {
        lv_timer_delete(s_zero_timer);
        s_zero_timer = NULL;
    }
    /* 【v0.6.0】WiFi 页的扫描轮询定时器与密码浮层同样必须随页失效：
     * 定时器若留着，会在页面关闭后继续轮询并往已删除的列表里写对象。 */
    if (s_wifi_scan_timer != NULL) {
        lv_timer_delete(s_wifi_scan_timer);
        s_wifi_scan_timer = NULL;
    }
    passkey_close();
    numkey_close(); /* v0.6.0：数字键盘浮层也随页失效（否则会浮在已关闭的页面上） */
    s_wifi_list = NULL;
    s_wifi_scan_btn = NULL;
    memset(s_wifi_mode_btns, 0, sizeof(s_wifi_mode_btns));
    s_wifi_ap_copy_n = 0;
    if (s_subpage != NULL) {
        /* lv_obj_delete()：删除对象并连带删除其所有子对象（标题栏/内容区在内） */
        lv_obj_delete(s_subpage);
    }
    /* 告警设置页：离开时请求立即落盘（settings_set_alarm 本身是防抖落盘，
     * 这里补一次"尽快写"以保证退出即写入、掉电不丢）。
     * 【v0.6.0 卡死修复】改用**非阻塞**的 settings_request_save()：
     * 本函数会被 subpage_open() 在每次开页前调用，且运行在持有 LVGL 锁的
     * UI 主循环里；原先的 settings_save_now() 会在锁内直接 nvs_commit
     * （数百 ms），期间整屏冻结（指示灯仍闪）。request 版本只置标志并唤醒
     * 后台落盘任务，写完即返回。 */
    if (s_sub_kind == SUB_ALARM) {
        (void)settings_request_save();
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
    s_wifi_state_label = NULL;
    s_wifi_hint_label = NULL;
    memset(s_alarm_value_labels, 0, sizeof(s_alarm_value_labels));
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
    /* 【v0.6.0 卡死定位】二级页进出日志：默认 DEBUG 级不输出，
     * 排查 UI 冻结时用 esp_log_level_set("set_ui", ESP_LOG_DEBUG) 打开，
     * 即可在串口看到"卡死前最后停在哪个子页"（配合 app_tasks 的渲染停滞告警）。 */
    ESP_LOGD(TAG, "进入二级页：%s", title);
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
    press_feedback_styles(back); /* 按下反馈与普通按钮一致（Bug2） */
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
    /* 【v0.6.0 卡死修复】必须用 **deferred** 版本：本回调是 LVGL 事件回调，
     * 运行在 UI 主循环里且此时**整轮持有 LVGL 锁**（见 ui.c 主循环）。
     * 同步版本会在锁内直接写 NVS（nvs_commit 可达数百 ms，还会与 task_aux
     * 的周期落盘争 s_save_mutex 叠加），期间 LVGL 渲染任务拿不到锁 →
     * 整屏冻结，而指示灯由独立 esp_timer 驱动照常闪烁 —— 这正是用户上报的
     * "进校准页后 UI 卡死、指示灯正常"。deferred 版本只改内存并置待落盘标志，
     * 真正的 Flash 写入由 task_aux 稍后异步完成。 */
    if (power_measure_set_calibration_deferred(&cal) != ESP_OK) {
        /* deferred 版本只在参数非法（增益 ≤0）时失败——校准流程已保证比值
         * 在 [0.5,2]，走到这里说明内存中的标定值已异常，提示用户重试 */
        show_notice("校准失败", "参数非法，请重试。");
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
 * @brief 电流零点一键清零（空载校零，采样按下后 1 秒内的平均值）
 *
 * 输出空载时点击：把电流零偏 offset 调到「抵消采样平均值」的位置，使显示
 * 立即归零。物理量 = raw×gain + offset，故
 *   offset_new = offset_old − 采样平均值
 *
 * 【2026-10 需求更新】原先用点击瞬间的单帧快照当清零值——单帧含噪声，
 * 偶发的坏帧/毛刺会被永久烧进零偏。改为「按下后 1 秒内的平均值」：
 *   · 异步实现（lv_timer 轮询 data_store），不阻塞 UI 任务/LVGL 锁；
 *   · 采样期间显示"正在采样"提示，结束弹结果（触摸/按键共用本回调）；
 *   · 均值约 40 帧（40Hz 快照），配合快照本身的掐头去尾，鲁棒性远高于单帧。
 *
 * @param e LVGL 点击事件（未使用）
 */
/* 清零采样定时器声明见文件前部（s_close_timer 旁） */
static float s_zero_sum = 0.0f;          /**< 清零采样电流累计（A） */
static uint32_t s_zero_n = 0;            /**< 清零采样帧数 */
static int64_t s_zero_start_us = 0;      /**< 清零采样起点（µs） */

static void zero_sample_timer_cb(lv_timer_t *t)
{
    (void)t;
    const int64_t now_us = esp_timer_get_time();

    /* 采样窗口内：累计每帧电流快照（seqlock 读，非阻塞） */
    data_store_snapshot_t snap;
    if (data_store_get(&snap) == ESP_OK) {
        s_zero_sum += snap.current_a;
        s_zero_n++;
    }

    if (now_us - s_zero_start_us < 1000000) {
        return; /* 1 秒窗口未满，继续 */
    }

    /* 窗口结束：自删定时器（lv_timer 删除自身在回调内是合法的） */
    lv_timer_del(s_zero_timer);
    s_zero_timer = NULL;

    if (s_zero_n == 0) {
        show_notice("清零失败", "采样期间无数据，请重试。");
        return;
    }
    const float avg = s_zero_sum / (float)s_zero_n;

    /* offset 调整：抵消采样平均值 */
    power_measure_calib_t cal;
    power_measure_get_calibration(&cal);
    cal.current_offset -= avg;
    /* 【v0.6.0 卡死修复】必须用 deferred 版本：本回调是 **LVGL 定时器回调**，
     * 运行在持有 LVGL 锁的 UI 主循环里。同步版 power_measure_set_calibration()
     * 会在锁内直接写 NVS（nvs_commit 可达数百 ms，还会与 task_aux 的周期落盘
     * 抢同一把 s_save_mutex 而叠加成秒级等待），期间 LVGL 渲染任务拿不到锁 →
     * 整屏冻结，而指示灯由独立 esp_timer 驱动照常闪烁。deferred 版只改内存
     * 标定值并置"待落盘"原子标志，真正的 Flash 写入交给 task_aux 异步完成。 */
    if (power_measure_set_calibration_deferred(&cal) != ESP_OK) {
        show_notice("清零失败", "参数保存失败，请重试。");
        return;
    }
    char text[96];
    snprintf(text, sizeof(text), "电流零点已校准。\n（采样 %.1fs / %u 帧均值\n %.4f A → 0）",
             (double)((now_us - s_zero_start_us) / 1000) / 1000.0, (unsigned)s_zero_n,
             (double)avg);
    show_notice("已清零", text);
}

static void calib_zero_cb(lv_event_t *e)
{
    (void)e;
    /* 重复点击保护：上一轮采样未结束时忽略 */
    if (s_zero_timer != NULL) {
        return;
    }
    s_zero_sum = 0.0f;
    s_zero_n = 0;
    s_zero_start_us = esp_timer_get_time();
    /* 40ms 轮询（≈40Hz 快照节奏，1 秒约 25 帧） */
    s_zero_timer = lv_timer_create(zero_sample_timer_cb, 40, NULL);
    show_notice("正在清零", "采样空载电流中…\n请保持空载约 1 秒。");
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
    /* settings_set_orientation(自动标志, 角度)：经 settings 封装只改内存缓存并标脏；
     * 【v0.6.0 卡死修复】改用非阻塞的 settings_request_save()：本回调在 LVGL 锁内
     * 运行，原先的 settings_save_now() 会当场 nvs_commit（数百 ms）冻结整屏。
     * request 版只置脏标志并唤醒后台落盘任务，1.5s 内异步写完（掉电窗口极小）。 */
    (void)settings_set_orientation(0, deg);
    (void)settings_request_save();
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
    /* 【v0.6.0 卡死修复】非阻塞落盘：见上面 angle_clicked_cb 的说明 */
    (void)settings_request_save();
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
        /* 用户松手即请求落盘（非阻塞，避免马上关机导致设置丢失）。
         * 【v0.6.0 卡死修复】不能用 settings_save_now()：松手事件在 LVGL 锁内派发，
         * 同步 nvs_commit 会冻结整屏数百 ms。 */
        (void)settings_request_save();
    }
}

/** 自动息屏档位名称表（下标 = settings.screen_off_s：0=永不 1=30s 2=60s 3=120s） */
static const char *const s_scr_off_names[4] = { "永不", "30 秒", "60 秒", "120 秒" };

/**
 * @brief "自动息屏"按钮：档位循环切换（永不 → 30s → 60s → 120s → 永不）
 *
 * 档位含义见 settings.screen_off_s（F4）：ui.c 主循环按"档位×30s 无输入"熄背光，
 * 任意按键唤醒。与"步长"按钮同款做法：直接改按钮自己的文字标签。
 *
 * @param e LVGL 点击事件（目标是"自动息屏"按钮）
 */
static void screen_off_cycle_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    settings_t cfg;
    uint8_t level = 0;
    if (settings_get(&cfg) == ESP_OK) {
        level = (cfg.screen_off_s < 4u) ? cfg.screen_off_s : 0u;
    }
    level = (uint8_t)((level + 1u) % 4u); /* 取模循环：永不→30→60→120→永不 */
    (void)settings_set_screen_off(level);
    (void)settings_request_save(); /* 【v0.6.0】非阻塞落盘，防在 LVGL 锁内冻屏 */
    lv_obj_t *lbl = lv_obj_get_child(btn, 0); /* 按钮内第 0 个子对象 = 文字标签 */
    if (lbl != NULL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "自动息屏：%s", s_scr_off_names[level]);
        lv_label_set_text(lbl, buf);
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

    /* ---- 自动息屏（F4）：档位循环切换按钮（永不/30s/60s/120s） ---- */
    uint8_t off_level = 0;
    if (settings_get(&cfg) == ESP_OK) {
        off_level = (cfg.screen_off_s < 4u) ? cfg.screen_off_s : 0u;
    }
    char off_buf[32];
    snprintf(off_buf, sizeof(off_buf), "自动息屏：%s", s_scr_off_names[off_level]);
    lv_obj_t *off_btn = plain_button(c, off_buf, LV_PCT(60), 30);
    lv_obj_add_event_cb(off_btn, screen_off_cycle_cb, LV_EVENT_CLICKED, NULL);
    focus_add(off_btn); /* 焦点链第 3 项：自动息屏档位（长按 K2 循环切换） */

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
        focus_add(b);        /* 焦点链第 4~7 项：0/90/180/270° 角度按钮 */
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
        (void)settings_request_save(); /* 【v0.6.0】非阻塞落盘（原 settings_save_now 会冻屏） */
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
 * 二级页：WiFi 状态与配网（F7 真实状态页）
 * ============================================================ */

/** WiFi 状态中文名（下标 = wifi_manager_state_t 枚举值，五态一一对应） */
static const char *const s_wifi_state_names[5] = {
    "未启动", "连接中", "已连接", "配网模式", "连接失败",
};

/* ---- v0.6.0 改版新增：模式切换 / 扫描列表 / 键盘配网 ---- */

/** 前向声明（本区内互相引用） */
static void wifi_update_status(void);
static void passkey_open(const char *ssid);
static void passkey_show_result(void);
static void passkey_submit(void);

/**
 * @brief 在父对象里画一组"4 格信号强度条"（按 RSSI 点亮）
 *
 * 用 4 个 lv_obj 小矩形而不是字符符号：完全不依赖字体子集，
 * 从根上规避"缺字方框"问题（本项目历史教训）。列表行与状态栏共用同一分档
 * （wifi_manager_rssi_level），保证两处显示一致。
 *
 * @param parent   父对象
 * @param rssi     信号强度（dBm）
 * @param on_color 点亮格的颜色
 */
static void wifi_sig_bars(lv_obj_t *parent, int8_t rssi, lv_color_t on_color)
{
    lv_obj_t *grp = plain_container(parent);
    lv_obj_set_size(grp, 26, 14);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grp, 2, 0);

    const int lvl = wifi_manager_rssi_level(rssi);
    static const int bar_h[4] = { 5, 8, 11, 14 }; /* 由矮到高，视觉上像信号条 */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(grp);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 4, bar_h[i]);
        lv_obj_set_style_bg_color(b, (i < lvl) ? on_color : COL_BORDER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_clickable(b, false); /* 纯显示，不拦截触摸 */
    }
}

/* ---- 模式三档按钮 ---- */

/** 刷新三档模式按钮的高亮（当前模式 = 品牌蓝底白字，其余白底灰字） */
static void wifi_mode_btns_refresh(void)
{
    const int cur = (int)wifi_manager_get_mode();
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = s_wifi_mode_btns[i];
        if (b == NULL) {
            continue;
        }
        lv_obj_t *lbl = lv_obj_get_child(b, 0);
        const bool on = (i == cur);
        lv_obj_set_style_bg_color(b, on ? COL_PRIMARY : lv_color_white(), 0);
        lv_obj_set_style_border_color(b, on ? COL_PRIMARY : COL_BORDER, 0);
        if (lbl != NULL) {
            lv_obj_set_style_text_color(lbl, on ? lv_color_white() : COL_INK, 0);
        }
    }
}

/**
 * @brief 模式按钮点击 → 请求切换工作模式（v0.6.0）
 *
 * wifi_manager_set_mode() 是**非阻塞**的（写队列即返回），因此本回调
 * 不会冻结界面；真正的 NVS 写与 esp_wifi 模式切换由 wifi_manager
 * 内部任务完成，界面靠 5Hz 状态刷新自动跟上。
 *
 * @param e LVGL 点击事件；user_data 携带目标模式（wifi_manager_mode_t）
 */
static void wifi_mode_clicked_cb(lv_event_t *e)
{
    const int mode = (int)(intptr_t)lv_event_get_user_data(e);
    if (mode < 0 || mode > (int)WIFI_MANAGER_MODE_AP) {
        return;
    }
    if (wifi_manager_set_mode((wifi_manager_mode_t)mode) == ESP_OK) {
        (void)buzzer_play(BUZZER_TONE_CLICK);
    }
    wifi_mode_btns_refresh(); /* 立即回显（乐观更新，状态刷新会再纠正一次） */
    wifi_update_status();
}

/* ---- 扫描结果列表 ---- */

/**
 * @brief 点击某一行热点 → 打开密码输入浮层
 *
 * @param e LVGL 点击事件；user_data 携带该行在 s_wifi_ap_copy 里的下标
 */
static void wifi_ap_row_clicked_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)s_wifi_ap_copy_n) {
        return;
    }
    /* esp_wifi 的 ssid 字段是 uint8_t 数组但内容为 ASCII/UTF-8 文本 */
    passkey_open((const char *)s_wifi_ap_copy[idx].ssid);
}

/**
 * @brief 渲染扫描结果列表（扫描完成后调用一次）
 *
 * 取 wifi_manager 的扫描缓存（最多 WIFI_UI_LIST_MAX 条），按 RSSI 降序
 * 排列后逐行创建。行高 32px：触屏目标达标，且 12 行可在内容区滚动查看。
 * 每行都登记到焦点链，纯按键环境下也能用 K1/K2 选中并打开密码框。
 */
static void wifi_render_ap_list(void)
{
    if (s_wifi_list == NULL) {
        return;
    }
    /* lv_obj_clean：删除容器的所有子对象（容器本身保留），用于重新渲染 */
    lv_obj_clean(s_wifi_list);

    uint16_t n = WIFI_UI_LIST_MAX;
    if (wifi_manager_scan_get_ap_list(s_wifi_ap_copy, &n) != ESP_OK) {
        n = 0; /* 没有结果（含扫描到 0 个热点） */
    }
    /* 按 RSSI 降序：简单的插入排序（n ≤ 12，开销可忽略） */
    for (uint16_t i = 1; i < n; i++) {
        wifi_ap_record_t key = s_wifi_ap_copy[i];
        int j = (int)i - 1;
        while (j >= 0 && s_wifi_ap_copy[j].rssi < key.rssi) {
            s_wifi_ap_copy[j + 1] = s_wifi_ap_copy[j];
            j--;
        }
        s_wifi_ap_copy[j + 1] = key;
    }
    s_wifi_ap_copy_n = n;

    if (n == 0) {
        lv_obj_t *none = plain_label(s_wifi_list, "未找到热点：请靠近路由器后重试。",
                                     LVGL_PORT_FONT_12, COL_GRAY, false);
        lv_obj_set_width(none, LV_PCT(100));
        lv_obj_set_style_text_align(none, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (uint16_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_button_create(s_wifi_list);
        lv_obj_set_size(row, LV_PCT(100), 32);
        press_feedback_styles(row);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        /* 左：热点名（长名截断，右侧控件优先保证可见） */
        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, (const char *)s_wifi_ap_copy[i].ssid);
        lv_obj_set_style_text_font(nm, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
        lv_obj_set_style_text_color(nm, COL_INK, 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_CLIP);
        lv_obj_set_flex_grow(nm, 1); /* 占满剩余宽度，把右侧组推到最右 */

        /* 右：加密标记 + 信号条 */
        lv_obj_t *rt = plain_container(row);
        lv_obj_set_size(rt, LV_SIZE_CONTENT, 24);
        lv_obj_set_flex_flow(rt, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rt, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(rt, 8, 0);
        if (s_wifi_ap_copy[i].authmode != WIFI_AUTH_OPEN) {
            /* 用文字"加密"而不是锁形符号：符号字形不在中文字体子集内，
             * 用符号会出现缺字方框（本项目历史问题），文字则一定在子集里 */
            plain_label(rt, "加密", LVGL_PORT_FONT_12, COL_GRAY, false);
        }
        wifi_sig_bars(rt, s_wifi_ap_copy[i].rssi, COL_OK);

        lv_obj_add_event_cb(row, wifi_ap_row_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        focus_add(row); /* 焦点链：逐行可选中（长按 K2 = 打开密码框） */
    }
}

/**
 * @brief 扫描轮询定时器：扫描结束即取结果、渲染列表、自删
 *
 * 用轮询而不是回调：wifi_manager 通过 wifi_manager_scan_is_running()
 * 暴露"扫描中"标记，UI 每 300ms 问一次即可，避免再给 middleware 加通知机制。
 *
 * @param t 定时器（到点后自删）
 */
static void wifi_scan_timer_cb(lv_timer_t *t)
{
    if (wifi_manager_scan_is_running()) {
        return; /* 还在扫，下一拍再问 */
    }
    /* 扫完了：删定时器、恢复按钮、渲染列表 */
    lv_timer_delete(t);
    s_wifi_scan_timer = NULL;
    if (s_wifi_scan_btn != NULL) {
        lv_obj_remove_state(s_wifi_scan_btn, LV_STATE_DISABLED); /* 恢复可点 */
        lv_obj_t *l = lv_obj_get_child(s_wifi_scan_btn, 0);
        if (l != NULL) {
            lv_label_set_text(l, "重新扫描"); /* 从"扫描中…"恢复 */
        }
    }
    wifi_render_ap_list();
    if (s_wifi_hint_label != NULL) {
        lv_label_set_text(s_wifi_hint_label, "点选热点后输入密码即可连接；"
                                              "也可用手机连热点配网。");
    }
}

/**
 * @brief 发起一次扫描并进入"扫描中"状态（按钮置灰 + 300ms 轮询定时器）
 *
 * 扫描由 wifi_manager 内部任务执行（esp_wifi_scan_start 异步），UI 只轮询
 * 结果；不会阻塞 LVGL。
 */
static void wifi_scan_begin(void)
{
    if (s_wifi_scan_timer != NULL) {
        return; /* 已在扫描中 */
    }
    esp_err_t err = wifi_manager_scan_start();
    if (err != ESP_OK) {
        /* 缓冲留足 160 字节：固定文案 + esp_err_to_name 的错误名（GCC 的
         * -Wformat-truncation 会按最坏情况核算，缓冲太紧会直接编译失败） */
        char msg[160];
        snprintf(msg, sizeof(msg), "无法发起扫描：%s\n请稍后重试，或改用热点配网。",
                 esp_err_to_name(err));
        show_notice("扫描失败", msg);
        return;
    }
    (void)buzzer_play(BUZZER_TONE_CLICK);
    if (s_wifi_scan_btn != NULL) {
        lv_obj_add_state(s_wifi_scan_btn, LV_STATE_DISABLED);
        lv_obj_t *l = lv_obj_get_child(s_wifi_scan_btn, 0);
        if (l != NULL) {
            lv_label_set_text(l, "扫描中…");
        }
    }
    if (s_wifi_hint_label != NULL) {
        lv_label_set_text(s_wifi_hint_label, "正在扫描附近热点…（约 3 秒）");
    }
    s_wifi_scan_timer = lv_timer_create(wifi_scan_timer_cb, 300, NULL);
}

/**
 * @brief "重新扫描"按钮（扫描子页内）：直接复用发起逻辑
 *
 * @param e LVGL 点击事件（未使用）
 */
static void wifi_rescan_clicked_cb(lv_event_t *e)
{
    (void)e;
    wifi_scan_begin();
}

/**
 * @brief 打开"WiFi 扫描"独立二级页（v0.6.1 需求 3）
 *
 * 【为什么从 WiFi 主页拆出来】旧版把 12 行结果列表内嵌在主页里，一页同时
 * 装下「状态 + 模式三档 + 两个操作按钮 + 12 行列表」：行对象一次创建约
 * 百个，LVGL 内存压力大（v0.6.0 概率卡死的帮凶之一），且主页信息密度
 * 过高。独立页给列表完整的高度（可滚动），扫描状态与结果展示更清晰。
 *
 * @param auto_start true = 打开后立即发起扫描（从主页按钮进入的常态）
 */
static void wifi_scan_page_open(bool auto_start)
{
    lv_obj_t *c = subpage_open(SUB_WIFI_SCAN, "WiFi 扫描");

    /* ① 状态/提示行（扫描中 → 结果摘要），复用 s_wifi_hint_label */
    s_wifi_hint_label = plain_label(c, auto_start ? "正在扫描附近热点…（约 3 秒）"
                                                  : "点「重新扫描」列出附近热点。",
                                    LVGL_PORT_FONT_12, COL_GRAY, false);
    lv_obj_set_width(s_wifi_hint_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_wifi_hint_label, LV_TEXT_ALIGN_CENTER, 0);

    /* ② 重新扫描按钮（主动态，品牌蓝） */
    s_wifi_scan_btn = plain_button(c, "重新扫描", LV_PCT(60), 34);
    lv_obj_set_style_bg_color(s_wifi_scan_btn, COL_PRIMARY, 0);
    lv_obj_set_style_border_color(s_wifi_scan_btn, COL_PRIMARY, 0);
    {
        lv_obj_t *l = lv_obj_get_child(s_wifi_scan_btn, 0);
        if (l != NULL) {
            lv_obj_set_style_text_color(l, lv_color_white(), 0);
            lv_obj_set_style_text_font(l, lvgl_port_get_font_bold(LVGL_PORT_FONT_14), 0);
        }
    }
    lv_obj_add_event_cb(s_wifi_scan_btn, wifi_rescan_clicked_cb, LV_EVENT_CLICKED, NULL);
    focus_add(s_wifi_scan_btn);

    /* ③ 结果列表（占满剩余高度，行内滚动；行由 wifi_render_ap_list 动态建） */
    s_wifi_list = plain_container(c);
    lv_obj_set_width(s_wifi_list, LV_PCT(100));
    lv_obj_set_height(s_wifi_list, 0);
    lv_obj_set_flex_grow(s_wifi_list, 1);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_wifi_list, 2, 0);
    lv_obj_set_style_pad_row(s_wifi_list, 4, 0);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_AUTO);

    s_wifi_ap_copy_n = 0;
    if (auto_start) {
        wifi_scan_begin();
    } else if (!wifi_manager_scan_is_running()) {
        /* 非自动发起且没有在扫：把上一次缓存的结果直接显示出来（若有） */
        wifi_render_ap_list();
        if (s_wifi_hint_label != NULL && s_wifi_ap_copy_n > 0) {
            lv_label_set_text(s_wifi_hint_label, "点选热点后输入密码即可连接。");
        }
    } else {
        /* 打开时已有扫描在跑：接上轮询（按钮置灰复用 wifi_scan_begin 的视觉） */
        if (s_wifi_scan_btn != NULL) {
            lv_obj_add_state(s_wifi_scan_btn, LV_STATE_DISABLED);
            lv_obj_t *l = lv_obj_get_child(s_wifi_scan_btn, 0);
            if (l != NULL) {
                lv_label_set_text(l, "扫描中…");
            }
        }
        s_wifi_scan_timer = lv_timer_create(wifi_scan_timer_cb, 300, NULL);
    }
}

/**
 * @brief "搜索 Wi-Fi"按钮（WiFi 主页）：跳转到独立扫描页并自动发起
 *
 * @param e LVGL 点击事件（未使用）
 */
static void wifi_scan_btn_clicked_cb(lv_event_t *e)
{
    (void)e;
    wifi_scan_page_open(true);
}

/* ---- 密码输入浮层（全屏 + 文字键盘） ---- */

/**
 * @brief 关闭密码输入浮层（无则空操作）
 */
static void passkey_close(void)
{
    if (s_passkey_page != NULL) {
        lv_obj_delete(s_passkey_page); /* 连带删除浮层内所有子对象（含键盘） */
        s_passkey_page = NULL;
    }
    s_passkey_ta = NULL;
}

/**
 * @brief "连接路由器"的一次性任务（**不在 LVGL 上下文**）
 *
 * wifi_manager_connect() 会等待其内部任务完成（最长数秒，含 NVS 写）。
 * 若在 LVGL 回调里直接调用会冻结界面，因此与"忘记网络"同款处理：
 * 本任务负责阻塞调用，完成后用 lv_async_call 排回 UI 弹结果。
 *
 * @param arg 未使用
 */
static void wifi_connect_task(void *arg)
{
    (void)arg;
    s_wifi_conn_result = wifi_manager_connect(s_conn_ssid, s_conn_pass);
    lv_async_call((lv_async_cb_t)passkey_show_result, NULL);
    vTaskDelete(NULL);
}

/**
 * @brief 连接结果收尾（**在 LVGL 上下文**执行）
 *
 * @param unused lv_async_call 的 user_data（未使用）
 */
static void passkey_show_result(void)
{
    s_wifi_conn_running = false;
    wifi_update_status();
    if (s_wifi_conn_result == ESP_OK) {
        buzzer_play(BUZZER_TONE_OK);
        show_notice("已提交连接",
                    "凭据已保存并开始连接。\n连上路由器后设备热点会关闭，\n"
                    "请用路由器分配的 IP 打开控制台。");
    } else {
        buzzer_play(BUZZER_TONE_ERROR);
        char msg[96];
        snprintf(msg, sizeof(msg), "连接请求失败：%s\n请检查密码或重试。",
                 esp_err_to_name(s_wifi_conn_result));
        show_notice("连接失败", msg);
    }
}

/**
 * @brief 密码框"连接"：取出输入内容并交给一次性任务
 */
static void passkey_submit(void)
{
    if ((s_passkey_ta == NULL) || s_wifi_conn_running) {
        return;
    }
    const char *pw = lv_textarea_get_text(s_passkey_ta);
    snprintf(s_conn_ssid, sizeof(s_conn_ssid), "%s", s_passkey_ssid);
    snprintf(s_conn_pass, sizeof(s_conn_pass), "%s", (pw != NULL) ? pw : "");
    passkey_close();
    s_wifi_conn_running = true;
    if (xTaskCreatePinnedToCore(wifi_connect_task, "wifi_conn", 3072, NULL, 4, NULL, 0) !=
        pdPASS) {
        s_wifi_conn_running = false;
        show_notice("操作未完成", "系统资源不足，请重试。");
        return;
    }
    if (s_wifi_hint_label != NULL) {
        lv_label_set_text(s_wifi_hint_label, "正在提交连接…");
    }
}

/** @brief 密码框"取消"按钮 / 返回键：直接关浮层 */
static void passkey_cancel_cb(lv_event_t *e)
{
    (void)e;
    passkey_close();
}

/** @brief "连接"按钮的 LVGL 事件薄封装（LVGL 回调签名带参数，业务函数不带） */
static void passkey_submit_cb(lv_event_t *e)
{
    (void)e;
    passkey_submit();
}

/**
 * @brief 打开密码输入浮层（v0.6.0）
 *
 * 为什么是"浮层"而不是 lv_msgbox：文字键盘约 120px 高，塞进 250px 宽的
 * 对话框会超出一屏高度；浮层挂在设置 tile 上（后创建者渲染在上层），
 * 尺寸可控且不会与弹窗/遮罩机制互相干扰。
 *
 * 版式（284×240，顶部 22px 让开共享状态栏）：
 *   标题（"连接 <SSID>"，16px 粗体）
 *   密码输入框（单行、密码模式、34px）
 *   lv_keyboard（TEXT_LOWER，116px）
 *   [取消] [连接]（各 50%，32px）
 *
 * @param ssid 目标热点名（浮层标题显示它，连接时使用它）
 */
static void passkey_open(const char *ssid)
{
    passkey_close(); /* 防重复打开 */
    snprintf(s_passkey_ssid, sizeof(s_passkey_ssid), "%s", (ssid != NULL) ? ssid : "");

    lv_obj_t *p = lv_obj_create(s_tile); /* 挂设置 tile：不跨页，随 tile 一起滑动 */
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_scroll_dir(p, LV_DIR_ALL); /* 吞掉滑动，防误翻页 */
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_top(p, 22, 0); /* 让开共享状态栏（22px） */
    lv_obj_set_style_pad_hor(p, 8, 0);
    lv_obj_set_style_pad_bottom(p, 4, 0);
    lv_obj_set_style_pad_row(p, 4, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_passkey_page = p;

    char title[48];
    snprintf(title, sizeof(title), "连接 %s", s_passkey_ssid);
    lv_obj_t *tl = plain_label(p, title, LVGL_PORT_FONT_16, COL_INK, true);
    lv_obj_set_width(tl, LV_PCT(100));
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT); /* 超长省略号，不断行占两行 */
    lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_CENTER, 0);

    /* 密码框：密码模式 + 自定义掩码字符。
     * 掩码必须用 ASCII "*"：LVGL 默认掩码是 U+2022（•），该字形不在本工程
     * 的中文字体子集里，真机可能显示成方框（字体审计会报缺失）。 */
    s_passkey_ta = lv_textarea_create(p);
    lv_obj_set_width(s_passkey_ta, LV_PCT(100));
    lv_obj_set_height(s_passkey_ta, 34);
    lv_textarea_set_one_line(s_passkey_ta, true);
    lv_textarea_set_password_mode(s_passkey_ta, true);
    lv_textarea_set_password_bullet(s_passkey_ta, "*");
    lv_textarea_set_max_length(s_passkey_ta, WIFI_MANAGER_PASS_MAX_LEN - 1);
    lv_obj_set_style_text_font(s_passkey_ta, lvgl_port_get_font(LVGL_PORT_FONT_16), 0);

    lv_obj_t *kb = lv_keyboard_create(p);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_height(kb, 100); /* 4 行按键 → 每键约 24px 高，且保证底部按钮在屏内 */
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, s_passkey_ta);
    lv_obj_set_style_text_font(kb, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);

    lv_obj_t *row = plain_container(p);
    lv_obj_set_size(row, LV_PCT(100), 32);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t *cancel = plain_button(row, "取消", LV_PCT(50), 32);
    lv_obj_add_event_cb(cancel, passkey_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok = plain_button(row, "连接", LV_PCT(50), 32);
    lv_obj_set_style_bg_color(ok, COL_PRIMARY, 0);
    lv_obj_set_style_border_color(ok, COL_PRIMARY, 0);
    lv_obj_t *okl = lv_obj_get_child(ok, 0);
    if (okl != NULL) {
        lv_obj_set_style_text_color(okl, lv_color_white(), 0);
        lv_obj_set_style_text_font(okl, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    }
    lv_obj_add_event_cb(ok, passkey_submit_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGD(TAG, "密码输入浮层已打开（SSID=%s）", s_passkey_ssid);
}


/**
 * @brief 刷新 WiFi 状态行与提示行（建页时与 5Hz 刷新各调一次）
 *
 * 状态行格式（wifi_manager_get_state 中文名 + IP 或热点名）：
 *   已连接   → "已连接 · IP x.x.x.x"（取不到 IP 显示 "--"）；
 *   配网模式 → "配网模式 · 热点 <wifi_manager_get_ap_ssid>"；
 *   连接中   → "连接中 · <已保存热点名>"（无凭据时只显示状态名）；
 *   未启动 / 连接失败 → 只显示状态名。
 * 提示行随状态切换：配网模式提示"手机连热点后浏览器开 192.168.4.1 配网"。
 */
static void wifi_update_status(void)
{
    /* 模式按钮高亮：每帧都按 wifi_manager 的真实模式回显（用户点了别处/重启后
     * 也能纠正"乐观更新"留下的偏差） */
    wifi_mode_btns_refresh();

    if (s_wifi_state_label == NULL) {
        return; /* 页已关闭：防野指针（与其余刷新标签同一约定） */
    }
    /* 【v0.6.0 复核】本函数以 5Hz 在"持有 LVGL 锁"的 UI 主循环里被调用，
     * 因此它调用的每个 wifi_manager 接口都必须是**非阻塞**的。
     * 已逐一核对实现（wifi_manager.c）：get_state/get_ip/get_ssid/get_ap_ssid
     * 全部是 portENTER_CRITICAL 保护的快照拷贝，不等待内部命令任务，
     * 不会阻塞 LVGL 锁。若将来给它们加"等待应答"的语义，必须回头改本函数。 */
    wifi_manager_state_t st = wifi_manager_get_state();
    const char *name = (st <= WIFI_MANAGER_STATE_FAILED) ? s_wifi_state_names[st] : "未知";
    char line[64];
    if (st == WIFI_MANAGER_STATE_CONNECTED) {
        char ip[16] = ""; /* wifi_manager_get_ip 至少 16 字节（含结尾 0） */
        if (wifi_manager_get_ip(ip, sizeof(ip)) != ESP_OK) {
            snprintf(ip, sizeof(ip), "--");
        }
        /* 带信号强度（dBm）：与列表里的信号条是同源数据，便于判断连接质量 */
        int8_t rssi = 0;
        if (wifi_manager_get_rssi(&rssi) == ESP_OK) {
            snprintf(line, sizeof(line), "%s · IP %s · %d dBm", name, ip, (int)rssi);
        } else {
            snprintf(line, sizeof(line), "%s · IP %s", name, ip);
        }
    } else if (st == WIFI_MANAGER_STATE_AP_MODE) {
        char ap[WIFI_MANAGER_AP_SSID_MAX_LEN + 1] = "";
        if (wifi_manager_get_ap_ssid(ap, sizeof(ap)) != ESP_OK) {
            snprintf(ap, sizeof(ap), "--");
        }
        snprintf(line, sizeof(line), "%s · 热点 %s", name, ap);
    } else if (st == WIFI_MANAGER_STATE_CONNECTING) {
        char ssid[WIFI_MANAGER_SSID_MAX_LEN + 1] = "";
        if (wifi_manager_get_ssid(ssid, sizeof(ssid)) == ESP_OK && ssid[0] != '\0') {
            snprintf(line, sizeof(line), "%s · %s", name, ssid);
        } else {
            snprintf(line, sizeof(line), "%s", name);
        }
    } else {
        snprintf(line, sizeof(line), "%s", name);
    }
    lv_label_set_text(s_wifi_state_label, line);

    if (s_wifi_hint_label != NULL) {
        if (st == WIFI_MANAGER_STATE_AP_MODE) {
            /* 配网指引：AP 模式下手机连热点 → 浏览器开 192.168.4.1 配网页 */
            lv_label_set_text(s_wifi_hint_label,
                              "手机连接上述热点后，\n浏览器打开 192.168.4.1 完成配网。");
        } else if (st == WIFI_MANAGER_STATE_FAILED &&
                   wifi_manager_get_mode() != WIFI_MANAGER_MODE_AP) {
            /* 仅 STA / 自动但无凭据：热点未开，提示先切到热点模式才能配网 */
            lv_label_set_text(s_wifi_hint_label,
                              "当前未连接且无凭据。\n切到「热点」模式即可用手机/电脑配网。");
        } else {
            lv_label_set_text(s_wifi_hint_label, "“忘记网络”将清除已保存的 WiFi 凭据。");
        }
    }
}

/** 忘记网络：一次性任务与 LVGL 异步回调之间的结果传递（同一时刻只允许一个在跑） */
static esp_err_t s_forget_result = ESP_OK;   /**< wifi_manager_forget() 的返回值 */
static bool s_forget_running = false;        /**< 已有一个"忘记网络"任务在跑（防重复点击） */

/**
 * @brief 忘记网络完成后的 UI 收尾（**在 LVGL 上下文**执行）
 *
 * 由 wifi_forget_task() 通过 lv_async_call() 排队到 LVGL 任务里执行，
 * 因此这里可以安全地操作 LVGL 对象（弹提示框/刷状态行）。
 *
 * @param unused lv_async_call 的 user_data（未使用）
 */
static void wifi_forget_done_async(void *unused)
{
    (void)unused;
    s_forget_running = false;
    wifi_update_status(); /* 先刷背后状态页，再弹结果框 */
    if (s_forget_result == ESP_OK) {
        buzzer_play(BUZZER_TONE_OK);
        /* 结果文案随模式偏好变化：仅 STA 模式下不会开热点，不能照旧说"回到热点" */
        if (wifi_manager_get_mode() == WIFI_MANAGER_MODE_STA) {
            show_notice("已忘记网络", "WiFi 凭据已清除。\n当前为「仅 STA」模式，\n"
                                      "需要重新配网时请切到「热点」模式。");
        } else {
            show_notice("已忘记网络", "WiFi 凭据已清除，\n设备回到配网热点模式。");
        }
    } else {
        buzzer_play(BUZZER_TONE_ERROR);
        show_notice("操作未完成", "清除 WiFi 凭据失败，请重试。");
    }
}

/**
 * @brief 忘记网络的一次性执行任务（**不在 LVGL 上下文**）
 *
 * 【v0.6.0 卡死修复】wifi_manager_forget() 会等待其内部任务完成（头文件
 * 注明"最长数秒"）。若直接在 LVGL 事件回调里调用，UI 主循环会持着 LVGL 锁
 * 干等数秒→整屏冻结（指示灯仍闪）。因此拆成两步：
 *   ① 本任务（普通 FreeRTOS 任务，不碰任何 LVGL 对象）执行阻塞操作；
 *   ② 完成后用 lv_async_call() 把结果排回 LVGL 任务里收尾。
 * 栈用内部 RAM（3KB）：本任务只做一次信号量等待 + 几个 WiFi API 调用，
 * 且 esp_wifi 的中断不依赖本任务栈（区别于必须内部栈的采样任务）。
 *
 * @param arg 未使用
 */
static void wifi_forget_task(void *arg)
{
    (void)arg;
    s_forget_result = wifi_manager_forget();
    /* lv_async_call：把回调排到 LVGL 任务上下文执行（跨线程安全） */
    lv_async_call(wifi_forget_done_async, NULL);
    vTaskDelete(NULL); /* 一次性任务：干完自删 */
}

/**
 * @brief "忘记网络"确认后动作：清 WiFi 凭据并回配网热点（F7）
 *
 * 由 show_confirm 的 ok_cb 调用（确认框已先被关闭）。**不再阻塞**：
 * 立即在状态行显示"正在忘记网络…"，真正的动作交给一次性任务，
 * 完成后弹结果框（见 wifi_forget_done_async）。
 */
static void wifi_forget_ok(void)
{
    if (s_forget_running) {
        return; /* 已有一次在跑：忽略重复点击，避免并发两次 forget */
    }
    s_forget_running = true;
    if (s_wifi_state_label != NULL) {
        lv_label_set_text(s_wifi_state_label, "正在忘记网络…");
    }
    if (xTaskCreatePinnedToCore(wifi_forget_task, "wifi_forget", 3072, NULL, 4, NULL, 0) != pdPASS) {
        s_forget_running = false;
        show_notice("操作未完成", "系统资源不足，请重试。");
    }
}

/**
 * @brief "忘记网络"按钮：破坏性操作先弹确认框（取消/确定）
 * @param e LVGL 点击事件（未使用）
 */
static void wifi_forget_clicked_cb(lv_event_t *e)
{
    (void)e;
    show_confirm("忘记 WiFi 网络？",
                 "将清除已保存的 WiFi 凭据，\n设备回到配网热点模式。\n"
                 "测量数据与标定参数不受影响。",
                 wifi_forget_ok);
}

/**
 * @brief 打开"WiFi 与热点"二级页（真实状态页：状态/IP 或热点名 + 忘记网络）
 *
 * 状态来自 wifi_manager（middleware 数据面）：未启动/连接中/已连接/配网模式/
 * 连接失败；已连接显示 IP、配网模式显示热点名与配网指引。
 * 纯按键可达：唯一可操作控件（忘记网络按钮）登记焦点链，
 * "返回"固定在标题栏（长按 K1），长按 K2 激活焦点按钮弹确认框。
 */
static void wifi_open_cb(void)
{
    lv_obj_t *c = subpage_open(SUB_WIFI, "WiFi 与热点");

    /* ---- ① 状态行：品牌蓝加粗（本页主信息），由 wifi_update_status 5Hz 刷新 ---- */
    s_wifi_state_label = plain_label(c, "--", LVGL_PORT_FONT_14, COL_PRIMARY, true);
    lv_obj_set_width(s_wifi_state_label, LV_PCT(100));
    lv_label_set_long_mode(s_wifi_state_label, LV_LABEL_LONG_WRAP); /* 超宽折行不截断 */
    lv_obj_set_style_text_align(s_wifi_state_label, LV_TEXT_ALIGN_CENTER, 0);

    /* ---- ② 模式三档：自动 / 仅 STA / 热点（v0.6.0 需求 2） ----
     * 点击即 wifi_manager_set_mode（非阻塞），高亮由 wifi_mode_btns_refresh 维护。
     * 三档语义见 wifi_manager.h：自动=有凭据连路由器、无凭据开热点；
     * 仅 STA=不开热点（纯客户端）；热点=强制配网热点。 */
    lv_obj_t *mrow = plain_container(c);
    lv_obj_set_size(mrow, LV_PCT(100), 30);
    lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(mrow, 6, 0);
    static const char *const mode_names[3] = { "自动", "仅 STA", "热点" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = plain_button(mrow, mode_names[i], LV_PCT(33), 30);
        lv_obj_add_event_cb(b, wifi_mode_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        focus_add(b); /* 焦点链：三档模式按钮在最前（最常用） */
        s_wifi_mode_btns[i] = b;
    }

    /* ---- ③ 操作行：搜索 Wi-Fi / 忘记网络 ---- */
    lv_obj_t *arow = plain_container(c);
    lv_obj_set_size(arow, LV_PCT(100), 32);
    lv_obj_set_flex_flow(arow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(arow, 8, 0);

    s_wifi_scan_btn = plain_button(arow, "搜索 Wi-Fi", LV_PCT(50), 32);
    lv_obj_set_style_bg_color(s_wifi_scan_btn, COL_PRIMARY, 0);
    lv_obj_set_style_border_color(s_wifi_scan_btn, COL_PRIMARY, 0);
    {
        lv_obj_t *l = lv_obj_get_child(s_wifi_scan_btn, 0);
        if (l != NULL) {
            lv_obj_set_style_text_color(l, lv_color_white(), 0);
            lv_obj_set_style_text_font(l, lvgl_port_get_font_bold(LVGL_PORT_FONT_14), 0);
        }
    }
    lv_obj_add_event_cb(s_wifi_scan_btn, wifi_scan_btn_clicked_cb, LV_EVENT_CLICKED, NULL);
    focus_add(s_wifi_scan_btn);

    lv_obj_t *fbtn = plain_button(arow, "忘记网络", LV_PCT(50), 32);
    lv_obj_set_style_border_color(fbtn, COL_DANGER, 0);
    {
        lv_obj_t *l = lv_obj_get_child(fbtn, 0);
        if (l != NULL) {
            lv_obj_set_style_text_color(l, COL_DANGER, 0);
            lv_obj_set_style_text_font(l, lvgl_port_get_font_bold(LVGL_PORT_FONT_14), 0);
        }
    }
    lv_obj_add_event_cb(fbtn, wifi_forget_clicked_cb, LV_EVENT_CLICKED, NULL);
    focus_add(fbtn);

    /* ---- ④ 扫描结果改为独立二级页（v0.6.1 需求 3）：主页只留入口按钮，
     * 12 行列表不再内嵌本页——降低本页对象密度，列表也有完整可滚高度。 ---- */

    /* ---- ⑤ 提示行：随状态切换（配网指引 / 忘记网络说明） ---- */
    s_wifi_hint_label = plain_label(c, "", LVGL_PORT_FONT_12, COL_GRAY, false);
    lv_obj_set_width(s_wifi_hint_label, LV_PCT(100));
    lv_label_set_long_mode(s_wifi_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_wifi_hint_label, LV_TEXT_ALIGN_CENTER, 0);

    s_wifi_ap_copy_n = 0; /* 新开页：清掉上一次的行下标映射 */
    wifi_update_status(); /* 建页即填状态与模式高亮，不等 5Hz 刷新的第一帧 */
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
 * 数字键盘弹窗（v0.6.0 新增：点击数值直接输入）
 * ============================================================ */

/** 用户按下"确定"后的回调：拿到已校验并在范围内的数值 */
typedef void (*numkey_ok_cb_t)(float value);

/* 同一时刻只允许一个数字键盘弹窗，故这些状态用文件级静态变量即可 */
static lv_obj_t *s_numkey_box = NULL;   /**< 弹窗本体（lv_msgbox）；NULL=未打开 */
static lv_obj_t *s_numkey_ta = NULL;    /**< 弹窗内的输入框 */
static lv_obj_t *s_numkey_hint = NULL;  /**< 校验失败提示行（复用正文区最后一行） */
static numkey_ok_cb_t s_numkey_ok = NULL; /**< 确定回调 */
static float s_numkey_min = 0.0f;       /**< 允许的最小值（越界钳制） */
static float s_numkey_max = 0.0f;       /**< 允许的最大值（越界钳制） */

static void numkey_close(void); /* 前向声明（定义见下） */

/**
 * @brief 弹窗被删除时的清理（含用户点遮罩/按 K1 关闭的路径）
 *
 * 与 box_deleted_cb 类似：对象没了，登记的状态必须一并失效，
 * 否则下次 numkey_open 会往已删除的输入框写字（野指针）。
 */
static void numkey_deleted_cb(lv_event_t *e)
{
    (void)e;
    s_numkey_box = NULL;
    s_numkey_ta = NULL;
    s_numkey_hint = NULL;
    s_numkey_ok = NULL;
}

/**
 * @brief 数字键盘弹窗："确定"点击 → 校验 → 关闭 → 回调
 *
 * 校验失败的两种情形都**不关框**，只在提示行给反馈，让用户接着改：
 *   · 空串 / 非数字（含多个小数点）→ 提示"请输入数字"；
 *   · 超出范围 → 直接钳到边界值（与 ± 步进越界即停的语义一致）。
 *
 * @param e LVGL 点击事件（未使用）
 */
static void numkey_ok_cb(lv_event_t *e)
{
    (void)e;
    if (s_numkey_ta == NULL) {
        return;
    }
    const char *t = lv_textarea_get_text(s_numkey_ta);
    char *end = NULL;
    float v = (t != NULL) ? strtof(t, &end) : 0.0f;
    /* 判据：非空、至少解析出一个数、且整串都被吃掉（拒绝 "1.2.3"）、且有限 */
    if ((t == NULL) || (t[0] == '\0') || (end == t) || (*end != '\0') || !isfinite(v)) {
        if (s_numkey_hint != NULL) {
            lv_label_set_text(s_numkey_hint, "请输入数字（例如 12.5）");
        }
        return;
    }
    if (v < s_numkey_min) {
        v = s_numkey_min;
    }
    if (v > s_numkey_max) {
        v = s_numkey_max;
    }

    /* 回调指针先取出（关闭弹窗会把它清空），再关框、再执行动作 */
    numkey_ok_cb_t cb = s_numkey_ok;
    numkey_close();
    if (cb != NULL) {
        cb(v);
    }
}

/**
 * @brief 关闭数字键盘弹窗（无则空操作）
 */
static void numkey_close(void)
{
    if (s_numkey_box != NULL) {
        /* 浮层根对象删除：连带删除输入框与键盘（状态由 numkey_deleted_cb 清理） */
        lv_obj_delete(s_numkey_box);
    }
}

/**
 * @brief 数字键盘浮层"取消"：直接关闭（不改动任何值）
 */
static void numkey_cancel_cb(lv_event_t *e)
{
    (void)e;
    numkey_close();
}

/**
 * @brief 打开数字键盘浮层（点击数值直接输入，v0.6.0）
 *
 * 结构（284×240，顶部 22px 让开共享状态栏）：标题 + 范围提示 + 输入框 +
 * 数字键盘 + [取消][确定]。提示行兼职"校验失败反馈"（省一行高度）。
 *
 * 为什么用浮层而不是 lv_msgbox：数字键盘高约 92px，加上 msgbox 自带的
 * 标题/正文/按钮区会超过一屏高度（footer 按钮被挤出屏幕 = 点不到）。
 * 浮层挂在设置 tile 上（后创建者渲染在上层），高度可控、按钮必定可见。
 *
 * 键盘用 LVGL 内置 lv_keyboard 的 NUMBER 模式（4×4 数字键，每键约 60px 宽）。
 *
 * @param title  标题（如"输入过压阈值"）
 * @param unit   单位（如"V"；仅用于提示文案）
 * @param init   初始值（显示在输入框里）
 * @param min    允许最小值（确定时钳制）
 * @param max    允许最大值（确定时钳制）
 * @param cb     确定回调（已校验、已钳制的值）
 */
static void numkey_open(const char *title, const char *unit, float init, float min, float max,
                        numkey_ok_cb_t cb)
{
    numkey_close(); /* 防叠加 */

    s_numkey_min = min;
    s_numkey_max = max;
    s_numkey_ok = cb;

    lv_obj_t *p = lv_obj_create(s_tile); /* 全屏浮层（在二级页之上） */
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_scroll_dir(p, LV_DIR_ALL);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_top(p, 22, 0); /* 让开共享状态栏 */
    lv_obj_set_style_pad_hor(p, 8, 0);
    lv_obj_set_style_pad_bottom(p, 4, 0);
    lv_obj_set_style_pad_row(p, 4, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_numkey_box = p;
    lv_obj_add_event_cb(p, numkey_deleted_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *tl = plain_label(p, title, LVGL_PORT_FONT_16, COL_INK, true);
    lv_obj_set_width(tl, LV_PCT(100));
    lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_CENTER, 0);

    /* 提示行：初值是"允许范围"，校验失败时改写为错误提示（一行两用） */
    char text[64];
    snprintf(text, sizeof(text), "范围 %.3g ~ %.3g %s", (double)min, (double)max, unit);
    s_numkey_hint = plain_label(p, text, LVGL_PORT_FONT_12, COL_GRAY, false);
    lv_obj_set_width(s_numkey_hint, LV_PCT(100));
    lv_obj_set_style_text_align(s_numkey_hint, LV_TEXT_ALIGN_CENTER, 0);

    s_numkey_ta = lv_textarea_create(p);
    lv_obj_set_width(s_numkey_ta, LV_PCT(100));
    lv_obj_set_height(s_numkey_ta, 34);
    lv_textarea_set_one_line(s_numkey_ta, true);
    /* accepted_chars：只允许数字与小数点，键盘之外也挡得住误输入 */
    lv_textarea_set_accepted_chars(s_numkey_ta, "0123456789.");
    lv_textarea_set_max_length(s_numkey_ta, 9);
    lv_obj_set_style_text_font(s_numkey_ta, lvgl_port_get_font(LVGL_PORT_FONT_16), 0);
    {
        char init_buf[24];
        snprintf(init_buf, sizeof(init_buf), "%.4g", (double)init);
        lv_textarea_set_text(s_numkey_ta, init_buf);
    }

    lv_obj_t *kb = lv_keyboard_create(p);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_height(kb, 92); /* 4 行数字键 → 每键约 22px 高，恰好一屏放下 */
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, s_numkey_ta);
    lv_obj_set_style_text_font(kb, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);

    lv_obj_t *row = plain_container(p);
    lv_obj_set_size(row, LV_PCT(100), 32);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t *cancel = plain_button(row, "取消", LV_PCT(50), 32);
    lv_obj_add_event_cb(cancel, numkey_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok = plain_button(row, "确定", LV_PCT(50), 32);
    lv_obj_set_style_bg_color(ok, COL_PRIMARY, 0);
    lv_obj_set_style_border_color(ok, COL_PRIMARY, 0);
    {
        lv_obj_t *ol = lv_obj_get_child(ok, 0);
        if (ol != NULL) {
            lv_obj_set_style_text_color(ol, lv_color_white(), 0);
            lv_obj_set_style_text_font(ol, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
        }
    }
    lv_obj_add_event_cb(ok, numkey_ok_cb, LV_EVENT_CLICKED, NULL);
}

/* ============================================================
 * 二级页：告警设置（过压/过流/过功率阈值 + 使能开关）
 * ============================================================ */

/** 三行告警项下标 */
#define ALARM_ROW_V 0 /**< 过压（V） */
#define ALARM_ROW_I 1 /**< 过流（A） */
#define ALARM_ROW_P 2 /**< 过功率（W） */
#define ALARM_ROW_CNT 3

/** 阈值范围与步进（settings_set_alarm 要求阈值 >0，故下限取最小步进而非 0） */
#define ALARM_V_MIN 0.1f
#define ALARM_V_MAX 30.0f
#define ALARM_V_STEP 0.1f
#define ALARM_I_MIN 0.1f
#define ALARM_I_MAX 20.0f
#define ALARM_I_STEP 0.1f
#define ALARM_P_MIN 1.0f
#define ALARM_P_MAX 200.0f
#define ALARM_P_STEP 1.0f

/**
 * @brief 取某行阈值的范围/步进（集中一张表，避免三个回调各写一套）
 * @param row 行下标（ALARM_ROW_*）
 * @param min,max,step [out] 下限/上限/步进
 */
static void alarm_row_range(int row, float *min, float *max, float *step)
{
    switch (row) {
    case ALARM_ROW_V:
        *min = ALARM_V_MIN;
        *max = ALARM_V_MAX;
        *step = ALARM_V_STEP;
        break;
    case ALARM_ROW_I:
        *min = ALARM_I_MIN;
        *max = ALARM_I_MAX;
        *step = ALARM_I_STEP;
        break;
    default:
        *min = ALARM_P_MIN;
        *max = ALARM_P_MAX;
        *step = ALARM_P_STEP;
        break;
    }
}

/**
 * @brief 取某行当前阈值的指针（直接指向 s_alarm_cfg 内对应字段）
 * @param row 行下标（ALARM_ROW_*）
 * @return 阈值字段地址
 */
static float *alarm_row_value(int row)
{
    switch (row) {
    case ALARM_ROW_V:
        return &s_alarm_cfg.v_limit;
    case ALARM_ROW_I:
        return &s_alarm_cfg.i_limit;
    default:
        return &s_alarm_cfg.p_limit;
    }
}

/**
 * @brief 刷新某行阈值数值标签（"14.10 V" / "2.05 A" / "28.9 W"）
 * @param row 行下标（ALARM_ROW_*）
 */
static void alarm_update_label(int row)
{
    lv_obj_t *lbl = s_alarm_value_labels[row];
    if (lbl == NULL) {
        return;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f %s", (double)(*alarm_row_value(row)),
             row == ALARM_ROW_V ? "V" : (row == ALARM_ROW_I ? "A" : "W"));
    lv_label_set_text(lbl, buf);
}

/**
 * @brief 把当前编辑中的 s_alarm_cfg 应用到 settings（防抖落盘）
 *
 * 阈值已由 alarm_step_cb 钳到合法范围（>0），这里再兜一次底：
 * 任一项 ≤0（异常场景）则整体不写，保持原值不变。
 */
static void alarm_apply(void)
{
    if (!(s_alarm_cfg.v_limit > 0.0f) || !(s_alarm_cfg.i_limit > 0.0f) ||
        !(s_alarm_cfg.p_limit > 0.0f)) {
        return;
    }
    (void)settings_set_alarm(&s_alarm_cfg);
}

/**
 * @brief 阈值 ± 步进回调（"－"/"＋"按钮共用；user_data 编码行号与方向）
 *
 * user_data = row*2 + (dir>0 ? 1 : 0)：两个按钮共用一个回调，
 * 从 user_data 解出"哪一行、加还是减"（与校准向导的 ± 回调同思路）。
 * 步进后钳制到 [min,max]，越界即停（不回绕），随后刷新标签并落盘。
 *
 * @param e LVGL 点击事件
 */
static void alarm_step_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int row = code / 2;
    int dir = (code & 1) ? +1 : -1;
    if (row < 0 || row >= ALARM_ROW_CNT) {
        return;
    }
    float lo, hi, step;
    alarm_row_range(row, &lo, &hi, &step);
    float *v = alarm_row_value(row);
    float nv = *v + (float)dir * step;
    if (nv < lo) {
        nv = lo;
    }
    if (nv > hi) {
        nv = hi;
    }
    *v = nv;
    alarm_update_label(row);
    alarm_apply();
    (void)buzzer_play(BUZZER_TONE_CLICK); /* 步进反馈音（触屏/按键路径统一） */
}

/**
 * @brief 使能开关回调（翻转对应 v_en/i_en/p_en 并落盘）
 * @param e LVGL 值变化事件；user_data 携带行号（ALARM_ROW_*）
 */
static void alarm_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    if (row < 0 || row >= ALARM_ROW_CNT) {
        return;
    }
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    switch (row) {
    case ALARM_ROW_V:
        s_alarm_cfg.v_en = on ? 1u : 0u;
        break;
    case ALARM_ROW_I:
        s_alarm_cfg.i_en = on ? 1u : 0u;
        break;
    default:
        s_alarm_cfg.p_en = on ? 1u : 0u;
        break;
    }
    alarm_apply();
    (void)buzzer_play(BUZZER_TONE_CLICK);
}

/** 数字键盘正在编辑的行（-1 = 未在编辑）；供 alarm_numkey_ok 回写 */
static int s_numkey_alarm_row = -1;

/**
 * @brief 数字键盘"确定"→ 写回告警阈值并落盘
 *
 * @param v 已由 numkey_open 校验并钳制到该行范围内的值
 */
static void alarm_numkey_ok(float v)
{
    const int row = s_numkey_alarm_row;
    s_numkey_alarm_row = -1;
    if (row < 0 || row >= ALARM_ROW_CNT) {
        return;
    }
    *alarm_row_value(row) = v;
    alarm_update_label(row);
    alarm_apply(); /* 与 ± 步进同一出口：写内存 + 防抖落盘（非阻塞） */
    (void)buzzer_play(BUZZER_TONE_CLICK);
}

/**
 * @brief 点击阈值数值 → 弹出数字键盘直接输入（v0.6.0）
 *
 * 与 ± 步进按钮**并列**的两个入口：触屏用户可以直接输精确值，
 * 纯按键用户仍旧走 ± 步进（本标签不登记焦点链）。
 *
 * @param e LVGL 点击事件；user_data 携带行下标（ALARM_ROW_*）
 */
static void alarm_value_clicked_cb(lv_event_t *e)
{
    const int row = (int)(intptr_t)lv_event_get_user_data(e);
    if (row < 0 || row >= ALARM_ROW_CNT) {
        return;
    }
    float lo = 0.0f, hi = 0.0f, step = 0.0f;
    alarm_row_range(row, &lo, &hi, &step);
    const char *unit = (row == ALARM_ROW_V) ? "V" : ((row == ALARM_ROW_I) ? "A" : "W");
    char title[24];
    snprintf(title, sizeof(title), "输入%s阈值", (row == ALARM_ROW_V)   ? "过压"
                                           : (row == ALARM_ROW_I) ? "过流"
                                                                  : "过功率");
    s_numkey_alarm_row = row;
    numkey_open(title, unit, *alarm_row_value(row), lo, hi, alarm_numkey_ok);
}

/**
 * @brief 构造一行"阈值 ± 调节"（名称 + － + 数值 + ＋），并登记焦点链
 *
 * 行内 flex 横排：名称左对齐，± 与数值靠右成组。
 * 焦点链按"－ → ＋"逐行登记，纯按键路径就是 K1/K2 移焦点、长按 K2 点按钮。
 * 【v0.6.0】数值标签额外做成可点击：点击弹数字键盘直接输入（触屏入口），
 * 标签下加一条品牌蓝下划线作为"这里可以点"的视觉提示。
 *
 * @param c    二级页内容容器
 * @param row  行下标（ALARM_ROW_*）
 * @param name 行名称（"过压" 等）
 * @return 行容器（未用，便于将来扩展）
 */
static lv_obj_t *alarm_add_limit_row(lv_obj_t *c, int row, const char *name)
{
    lv_obj_t *r = plain_container(c);
    lv_obj_set_size(r, LV_PCT(100), 36);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    plain_label(r, name, LVGL_PORT_FONT_16, COL_INK, false);

    /* 右侧调节组：－ 数值 ＋ */
    lv_obj_t *grp = plain_container(r);
    lv_obj_set_size(grp, LV_SIZE_CONTENT, 36);
    lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grp, 4, 0);

    /* 注意：用全角"－/＋"（GB2312 子集内），不用 U+2212 数学减号（字库无字形）。
     * user_data = row*2 + 方向位，两个按钮共用 alarm_step_cb。 */
    lv_obj_t *minus = plain_button(grp, "－", 36, 32);
    lv_obj_add_event_cb(minus, alarm_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(row * 2));

    s_alarm_value_labels[row] = plain_label(grp, "--", LVGL_PORT_FONT_16, COL_PRIMARY, true);
    lv_obj_set_width(s_alarm_value_labels[row], 72);
    lv_obj_set_style_text_align(s_alarm_value_labels[row], LV_TEXT_ALIGN_CENTER, 0);
    /* 可点击 + 下划线提示（点击弹数字键盘，见 alarm_value_clicked_cb） */
    lv_obj_add_flag(s_alarm_value_labels[row], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_side(s_alarm_value_labels[row], LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_alarm_value_labels[row], 1, 0);
    lv_obj_set_style_border_color(s_alarm_value_labels[row], COL_PRIMARY, 0);
    lv_obj_set_style_pad_bottom(s_alarm_value_labels[row], 2, 0);
    lv_obj_add_event_cb(s_alarm_value_labels[row], alarm_value_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)row);
    alarm_update_label(row);

    lv_obj_t *plus = plain_button(grp, "＋", 36, 32);
    lv_obj_add_event_cb(plus, alarm_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(row * 2 + 1));

    focus_add(minus); /* 焦点链：－ → ＋（三行依次，见 alarm_open_cb 末尾说明） */
    focus_add(plus);
    return r;
}

/**
 * @brief 构造一行"使能开关"（名称 + lv_switch），并登记焦点链
 * @param c    二级页内容容器
 * @param row  行下标（ALARM_ROW_*）
 * @param name 行名称
 * @param on   初始开关态
 */
static void alarm_add_switch_row(lv_obj_t *c, int row, const char *name, bool on)
{
    lv_obj_t *r = plain_container(c);
    lv_obj_set_size(r, LV_PCT(100), 32);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    plain_label(r, name, LVGL_PORT_FONT_16, COL_INK, false);

    /* 开关样式与"显示设置"的自动旋转开关一致（白旋钮+深灰描边） */
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 46, 24);
    lv_obj_set_style_bg_color(sw, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sw, COL_PRIMARY, LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(sw, COL_GRAY, LV_PART_KNOB);
    lv_obj_set_style_border_width(sw, 1, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 3, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED); /* lv_obj_add_state：恢复持久化开关态 */
    }
    lv_obj_add_event_cb(sw, alarm_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)row);
    focus_add(sw); /* 焦点链：三开关（长按 K2 切换） */
}

/**
 * @brief 打开"告警设置"二级页（三阈值 ± 调节 + 三使能开关）
 *
 * 初值从 settings_get_alarm 读；读失败用安全默认（三阈值 50% 量程、全使能）。
 * 任一改动经 alarm_apply 写回 settings_set_alarm（防抖落盘），
 * 子页关闭时 subpage_close 还会请求一次尽快落盘（settings_request_save，非阻塞）。
 * 【v0.6.0】阈值数值可直接点击 → 弹数字键盘输入精确值（触屏入口）。
 */
static void alarm_open_cb(void)
{
    lv_obj_t *c = subpage_open(SUB_ALARM, "告警设置");

    if (settings_get_alarm(&s_alarm_cfg) != ESP_OK) {
        /* 读失败（未初始化等）：给一份安全默认值，页仍可用 */
        s_alarm_cfg.v_en = 1u;
        s_alarm_cfg.i_en = 1u;
        s_alarm_cfg.p_en = 1u;
        s_alarm_cfg.v_limit = 15.0f;
        s_alarm_cfg.i_limit = 10.0f;
        s_alarm_cfg.p_limit = 100.0f;
    }

    lv_obj_t *hint = plain_label(c, "阈值越限时测量页弹出告警；关掉开关即停用该项。\n"
                                    "点击数值可直接输入（触屏），或用 －/＋ 步进（按键）。",
                                 LVGL_PORT_FONT_12, COL_GRAY, false);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    /* 阈值三行：过压 → 过流 → 过功率 */
    alarm_add_limit_row(c, ALARM_ROW_V, "过压");
    alarm_add_limit_row(c, ALARM_ROW_I, "过流");
    alarm_add_limit_row(c, ALARM_ROW_P, "过功率");

    /* 使能三行：与阈值同序，方便"调完阈值顺手开关" */
    alarm_add_switch_row(c, ALARM_ROW_V, "过压使能", s_alarm_cfg.v_en != 0);
    alarm_add_switch_row(c, ALARM_ROW_I, "过流使能", s_alarm_cfg.i_en != 0);
    alarm_add_switch_row(c, ALARM_ROW_P, "过功率使能", s_alarm_cfg.p_en != 0);

    /* 焦点链顺序：过压－/＋ → 过流－/＋ → 过功率－/＋ → 三开关
     * （登记顺序即 K1/K2 遍历顺序，见 focus_add）。 */
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
 * 恢复出厂设置（F6，网格第 8 格入口）
 * ============================================================ */

/**
 * @brief 恢复出厂"确定"动作：清用户设置 → 电能清零 → 忘记 WiFi（逐项容错）
 *
 * 顺序固定：settings_restore_factory()（清 xt60cfg 用户设置回出厂默认；
 * 标定参数存 pm 命名空间，不受影响、有意保留）
 * → power_measure_reset_energy()（电能/运行时间清零）
 * → wifi_manager_forget()（清 WiFi 凭据并回配网热点）。
 * 三步相互独立，任一步失败不阻断其余步骤，最后按结果给提示框。
 * ⚠ wifi_manager_forget 会等待内部任务完成（最长数秒），期间界面短暂无响应，
 * 属一次性低频操作，可接受。
 */
static void factory_reset_ok(void)
{
    bool ok_set = (settings_restore_factory() == ESP_OK);
    bool ok_nrg = (power_measure_reset_energy() == ESP_OK);
    bool ok_wifi = (wifi_manager_forget() == ESP_OK);

    if (ok_set && ok_nrg && ok_wifi) {
        buzzer_play(BUZZER_TONE_OK);
        show_notice("已恢复出厂",
                    "用户设置、电能统计与 WiFi 网络已清除，\n标定参数已保留。");
    } else {
        /* 逐项容错：结果框把没做成的项目列出来，用户可单独重试 */
        char text[128];
        snprintf(text, sizeof(text), "以下项目未能完成：\n%s%s%s",
                 ok_set ? "" : "· 用户设置清除\n",
                 ok_nrg ? "" : "· 电能统计清零\n",
                 ok_wifi ? "" : "· WiFi 网络清除");
        show_notice("恢复出厂部分完成", text);
        buzzer_play(BUZZER_TONE_ERROR);
    }
}

/**
 * @brief 网格"恢复出厂"入口：破坏性操作先弹确认框（正文列清除项/保留项）
 *
 * 签名为 cell_cb_t（无参无返回），由 cell_clicked_cb 转发调用。
 */
static void factory_open_cb(void)
{
    show_confirm("恢复出厂设置？",
                 "将清除：\n· 用户设置\n· 电能统计\n· WiFi 网络\n"
                 "标定参数保留。\n此操作不可恢复！",
                 factory_reset_ok);
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
 * @param cb    点击后打开对应二级页的函数（NULL=留空位，不可点）
 * @return 创建好的单元格按钮（留空位返回 NULL，网格焦点据此跳过）
 */
static lv_obj_t *make_cell_dispatch(lv_obj_t *row, const char *icon, const char *name,
                                    cell_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_height(btn, CELL_HEIGHT);
    lv_obj_set_flex_grow(btn, 1);
    press_feedback_styles(btn); /* 按下反馈与普通按钮一致（Bug2） */

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 8, 0);

    /* 图标与文字同字号（16px），但图标符号来自 montserrat14 fallback，
     * 字形 box_h/ofs_y 与 Noto 主字体不同，同基线下图标比文字**低约 1.3px**
     * （3x 截图像素实测 1.2~1.3px）。translate_y -1 补偿后视觉中心对齐。 */
    lv_obj_t *icon_lbl = plain_label(btn, icon, LVGL_PORT_FONT_16, COL_PRIMARY, false);
    lv_obj_set_style_translate_y(icon_lbl, -1, 0);
    plain_label(btn, name, LVGL_PORT_FONT_16, COL_INK, false);

    /* 留空位（cb=NULL）：不可点、不登记回调，返回 NULL 让网格焦点跳过它 */
    if (cb == NULL) {
        lv_obj_set_clickable(btn, false); /* LVGL：不参与触摸命中测试 */
        return NULL;
    }
    lv_obj_add_event_cb(btn, cell_clicked_cb, LV_EVENT_CLICKED, (void *)cb);
    return btn;
}

/**
 * @brief 在设置 tile 上构建设置首页（2×4 功能网格）
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
    /* 【B12 修复】清零采样定时器随重建作废（见 subpage_close 同名注释） */
    if (s_zero_timer != NULL) {
        lv_timer_delete(s_zero_timer);
        s_zero_timer = NULL;
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

    /* 2×4 网格（7 个功能格 + 1 个留空位）：高度精确等于 4 行 + 3 行距，不给滚动留余地 */
    lv_obj_t *grid = lv_obj_create(tile);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), 4 * CELL_HEIGHT + 3 * CELL_GAP_Y);
    lv_obj_set_pos(grid, 0, GRID_TOP_Y);
    lv_obj_set_style_pad_left(grid, GRID_SIDE_PAD, 0);
    lv_obj_set_style_pad_right(grid, GRID_SIDE_PAD, 0);
    lv_obj_set_style_pad_row(grid, CELL_GAP_Y, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(grid, false);

    /* 功能格表：图标 + 名称 + 打开函数，按行序 r*2 / r*2+1 两两成行。
     * 8 个功能格占满 2×4 网格（第 8 格＝恢复出厂，F6）。
     * 替换某格功能只改这一行，排版代码不用动。 */
    static const struct {
        const char *icon;
        const char *name;
        cell_cb_t cb;
    } cells[GRID_CELL_COUNT] = {
        { LV_SYMBOL_SETTINGS, "测量设置", calib_ui_menu },
        { LV_SYMBOL_IMAGE, "显示设置", display_open_cb },
        { LV_SYMBOL_AUDIO, "蜂鸣器", buzzer_open_cb },
        { LV_SYMBOL_WIFI, "WiFi 与热点", wifi_open_cb },
        { LV_SYMBOL_REFRESH, "电能统计", energy_open_cb },
        { LV_SYMBOL_WARNING, "告警设置", alarm_open_cb },
        { LV_SYMBOL_LIST, "关于本机", about_open_cb },
        { LV_SYMBOL_TRASH, "恢复出厂", factory_open_cb },
    };

    for (int r = 0; r < 4; r++) {
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
    case SUB_WIFI:
        /* WiFi 状态页：状态/IP 随连接进度变化，5Hz 刷新保持"真实状态" */
        wifi_update_status();
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

const char *screen_settings_current_subpage_name(void)
{
    /* 返回静态字符串（不写共享缓冲）：调用方是 app_tasks 巡检任务，
     * 与 UI 任务并发，字符串常量才是安全的。校准页名随通道变化，
     * 只读一个 bool 标志（读到新旧值都无害）。 */
    switch (s_sub_kind) {
    case SUB_NONE:       return "首页";
    case SUB_CALIB_MENU: return "测量设置";
    case SUB_CALIB_EDIT: return s_cal_is_voltage ? "校准 · 电压" : "校准 · 电流";
    case SUB_CALIB_DONE: return "校准完成";
    case SUB_DISPLAY:    return "显示设置";
    case SUB_BUZZER:     return "蜂鸣器";
    case SUB_WIFI:       return "WiFi 与热点";
    case SUB_WIFI_SCAN:  return "WiFi 扫描";
    case SUB_ENERGY:     return "电能统计";
    case SUB_ALARM:      return "告警设置";
    default:             return "未知";
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
    /* passkey/numkey：v0.6.0 新增的两个全屏键盘浮层（含虚拟键盘），也是模态 */
    return (s_subpage != NULL || s_active_msgbox != NULL || s_passkey_page != NULL ||
            s_numkey_box != NULL);
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

    /* ---- ①b WiFi 密码输入浮层（v0.6.0）：K1=取消 / K2=连接 ----
     * 密码本身要靠触屏键盘输入（两键无法打字），但"取消/连接"必须可按键，
     * 否则浮层打开后按键全废、用户会以为界面卡死。 */
    if (s_passkey_page != NULL) {
        if (ev == BUTTON_EVENT_SINGLE_CLICK || ev == BUTTON_EVENT_LONG_PRESS_START) {
            if (id == BUTTON_ID_BOOT) {
                passkey_close(); /* K1 = 取消 */
            } else {
                passkey_submit(); /* K2 = 连接 */
            }
        }
        return true; /* 浮层期间按键不外漏 */
    }

    /* ---- ①c 数字键盘浮层（v0.6.0 告警阈值输入）：K1=取消 / K2=确定 ----
     * 数字靠触屏数字键输入；"取消/确定"可由按键完成（与弹窗键位一致）。 */
    if (s_numkey_box != NULL) {
        if (ev == BUTTON_EVENT_SINGLE_CLICK || ev == BUTTON_EVENT_LONG_PRESS_START) {
            if (id == BUTTON_ID_BOOT) {
                numkey_close(); /* K1 = 取消 */
            } else {
                /* K2 = 确定：合成点击"确定"按钮，走与触屏完全相同的出口 */
                lv_obj_t *box = s_numkey_box;
                lv_obj_t *row = lv_obj_get_child(box, lv_obj_get_child_count(box) - 1);
                if (row != NULL && lv_obj_get_child_count(row) >= 2) {
                    lv_obj_send_event(lv_obj_get_child(row, 1), LV_EVENT_CLICKED, NULL);
                }
            }
        }
        return true; /* 浮层期间按键不外漏 */
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
        /* 焦点上下移动（环形，跳过留空格）：K1=上一格、K2=下一格 */
        grid_focus_move(id == BUTTON_ID_BOOT ? -1 : +1);
        return true;
    case BUTTON_EVENT_LONG_PRESS_START:
        if (id == BUTTON_ID_USER) {
            /* 打开焦点功能格：合成 CLICKED 复用格子的打开回调（cell_clicked_cb），
             * 子页/弹窗随即接管按键（上面的 ①② 层）。
             * 留空格（NULL）正常不可达（grid_focus_move 已跳过），这里再兜一次。 */
            if (s_grid_cells[s_grid_idx] != NULL) {
                lv_obj_send_event(s_grid_cells[s_grid_idx], LV_EVENT_CLICKED, NULL);
            }
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