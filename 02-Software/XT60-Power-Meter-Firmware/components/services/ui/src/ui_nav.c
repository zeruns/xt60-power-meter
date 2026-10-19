/**
 * @file ui_nav.c
 * @brief 三页导航实现：tileview 三页 + 固定共享状态栏（含页码点）+ 按键路由
 *
 * 层级（都在 screen 上，z 序按创建顺序）：
 *   tileview（三页内容，可横滑）
 *   → 共享状态栏（白底浮层，不拦截触摸，三页始终可见）
 *
 * 状态栏内容（左→中→右）：运行时间 | 页码指示点 | WiFi + 电池。
 *   页码点原先放在屏幕底部中央，现移到状态栏中间——因为测量页要把环境
 *   温湿度显示移到底部居中，两者会重叠。
 *
 * 【核心机制：滑动方向判定为什么在 RELEASED 用总位移】
 *   需求：测量页/波形页上滑或下滑 → 呼出各自的快速菜单；左右滑翻页。
 *
 *   为什么不能用 LV_EVENT_SCROLL 的单帧增量（vect）判定方向：
 *     tileview 开了 LVGL 的滚动弹性（scroll elastic）——拖到边界时对象仍会
 *     被拖出边界，松手瞬间 LVGL 启动弹性回弹动画把位置拉回。回弹期间每帧的
 *     位移增量符号与用户手势方向相反且抖动剧烈，用单帧增量判方向会误判。
 *
 *   正确做法（本文件实现）：
 *     ① indev 级 LV_EVENT_PRESSED 记录按下起点，LV_EVENT_RELEASED 时用
 *        "抬起点 − 起点"的**总位移**判方向——总位移不受回弹动画影响，
 *        因为回弹发生在松手之后，不再改变抬起点坐标；
 *     ② 垂直为主 → 报上下滑呼出快速菜单；水平为主 → 交给 tileview 自身
 *        翻页（首末页到头即止，**不再环形翻页**，2026-10 需求调整）。
 *
 *   另注：LVGL 的 LV_EVENT_GESTURE 在本场景不可用（被 tileview 滚动抑制且
 *   要求瞬时速度阈值），故滑动识别全部走 indev 事件 + 手动向量计算。
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
/* wifi_manager：状态栏 WiFi 图标要显示真实连接状态与信号强度（v0.6.0 需求 4） */
#include "wifi_manager.h"

/* ---- 页码指示点几何（当前页为长条、其余为圆点）---- */
#define DOT_SIZE 6   /**< 非当前页圆点边长（px，正方形） */
#define DOT_W_CUR 16 /**< 当前页指示条宽度（px，拉长以示"在这里"） */
#define DOT_H_CUR 6  /**< 当前页指示条高度 */
#define DOT_GAP 7    /**< 相邻指示点间距（px） */
/** 状态栏内容左右内缩：面板顶部两角为圆角，文字必须离开圆角区域 */
#define TOPBAR_SAFE_X 28

/** 电池电量阈值（百分比）与对应颜色 */
#define BAT_COLOR_HIGH lv_color_hex(0x1E9E4F) /* ≥50 绿 */
#define BAT_COLOR_MID  lv_color_hex(0xE8A00C) /* 20~49 黄 */
#define BAT_COLOR_LOW  lv_color_hex(0xD0392B) /* 0~19 红 */
#define BAT_COLOR_NONE lv_color_hex(0xB9BFC7) /* 无数据 灰 */
#define BAT_FULL_MV 4150u  /**< 满电电压（mV，TP4057 充电截止约 4.15V） */
#define BAT_EMPTY_MV 3300u /**< 空电电压（mV，3.3V 以下视为没电） */

static lv_obj_t *s_tileview = NULL;           /**< 三页容器（lv_tileview 实例） */
static lv_obj_t *s_tiles[UI_PAGE_MAX];        /**< 三个 tile 页对象（下标 = ui_page_t） */
static lv_obj_t *s_dots[UI_PAGE_MAX];         /**< 状态栏页码指示点（下标 = ui_page_t） */
/** 共享状态栏（screen 级浮层，不随三页重建；重建后需重新置顶） */
static lv_obj_t *s_topbar = NULL;
static lv_obj_t *s_lbl_runtime; /**< 状态栏左侧"运行时间"label */
static lv_obj_t *s_lbl_bat;     /**< 状态栏右侧"电池图标+百分比"label */

/* ---- 状态栏 WiFi 信号条（v0.6.0 需求 4：图标随连接状态与信号强度变化） ----
 * 用 4 个 lv_obj 小矩形自绘，而不是 LVGL 的 WiFi 符号字形：符号只有一个
 * "有/无"形态，无法表达信号强弱，且新增字形还要动字体子集（缺字风险）。
 * 高度由矮到高、底部对齐，点亮格数由 wifi_manager_rssi_level() 分档给出。 */
#define WIFI_BAR_COUNT 4                        /**< 信号条格数 */
#define WIFI_BAR_W 3                            /**< 单格宽度（px） */
#define WIFI_BAR_GAP 2                          /**< 格间距（px） */
static lv_obj_t *s_wifi_bars[WIFI_BAR_COUNT] = {0}; /**< 4 个信号条对象 */
static const int s_wifi_bar_h[WIFI_BAR_COUNT] = { 4, 6, 9, 12 }; /**< 各格高度（矮→高） */

/** 信号条配色：与状态栏电池同风格（健康=绿、配网=品牌蓝、无=灰） */
#define WIFI_COLOR_ON   lv_color_hex(0x1E9E4F) /* 已连接（信号好/中） */
#define WIFI_COLOR_WEAK lv_color_hex(0xE8A00C) /* 已连接但信号弱 */
#define WIFI_COLOR_AP   lv_color_hex(0x0066CC) /* 配网热点模式 */
#define WIFI_COLOR_OFF  BAT_COLOR_NONE         /* 未连接/未启动 */
static ui_page_t s_current = UI_PAGE_MEASURE; /**< 当前页（scroll_event_cb 跟踪滚动实时更新） */

/**
 * @brief 电池电压（mV）→ 电量百分比（线性映射 3.3V~4.15V → 0~100）
 *
 * 满电取 4.15V：本机充电 IC 为 TP4057，充电截止电压约 4.15V，用 4.2V
 * 作满档会让充满后长期显示不满 100%（2026-10 实测）。
 *
 * @param mv 电池电压（mV）；0 表示无电池数据
 * @return 电量百分比（0~100）
 */
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

/**
 * @brief 按电量百分比选电池颜色（绿/黄/红），无数据显示灰色
 *
 * @param pct     电量百分比（0~100）
 * @param has_data 是否有电池电压数据（false = 显示灰色占位）
 * @return LVGL 颜色值
 */
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
 * @brief 页码指示点（放在状态栏中间）
 *
 * 原先是屏幕底部中央的独立浮层；测量页要在底部居中显示环境温湿度，
 * 故并入状态栏，避免与温湿度文字重叠。
 *
 * @param parent 父容器（状态栏）
 */
static void create_dots(lv_obj_t *parent)
{
    lv_obj_t *dotbar = lv_obj_create(parent); /* 创建一个通用容器对象 */
    lv_obj_remove_style_all(dotbar);          /* 清掉默认主题样式（边框/背景/内边距全去掉） */
    lv_obj_set_size(dotbar, DOT_W_CUR + 2 * (DOT_SIZE + DOT_GAP), DOT_H_CUR);
    /* 指示点是纯装饰：不可点击、不可滚动，避免吞掉滑向 tileview 的手势 */
    lv_obj_set_clickable(dotbar, false);
    lv_obj_set_scrollable(dotbar, false);
    /* lv_obj_set_flex_flow：启用 Flex 布局，ROW = 子对象水平排一行 */
    lv_obj_set_flex_flow(dotbar, LV_FLEX_FLOW_ROW);
    /* 三个方向（主轴/交叉轴/跨轴）都居中对齐 */
    lv_obj_set_flex_align(dotbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dotbar, DOT_GAP, 0); /* 子对象横向间距 */

    for (int i = 0; i < UI_PAGE_MAX; i++) {
        s_dots[i] = lv_obj_create(dotbar);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_clickable(s_dots[i], false);
        lv_obj_set_scrollable(s_dots[i], false);
        /* 当前页用宽条、其余用小圆点（update_dots 里随切页动态改尺寸） */
        lv_obj_set_size(s_dots[i], (i == UI_PAGE_MEASURE) ? DOT_W_CUR : DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0); /* 背景不透明（颜色由 update_dots 设） */
    }
}

/**
 * @brief 固定在屏幕顶部的共享状态栏（白底，触摸穿透，三页通用）
 *
 * 状态栏是 screen 的直接子对象、盖在 tileview 之上，所以翻页时始终可见；
 * 设为不可点击/不可滚动，触摸事件会穿透到下面的 tileview。
 *
 * @param scr 挂载父对象（当前活动屏）
 */
static void create_topbar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr); /* screen 级浮层，不放进 tileview */
    s_topbar = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 22); /* LV_PCT(100)：宽度取父对象的 100%（旋转后自适应） */
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0); /* 白底不透明，遮住下面滚过的内容 */
    lv_obj_set_clickable(bar, false);              /* 触摸穿透：不拦截滑动手势 */
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    /* SPACE_BETWEEN：左/中/右三组子对象两端对齐，中间的页码点自然居中 */
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    /* 内缩 28px：这块区域正好是顶部两个圆角，文字进去会被遮住 */
    lv_obj_set_style_pad_hor(bar, TOPBAR_SAFE_X, 0);

    s_lbl_runtime = lv_label_create(bar); /* 左侧：运行时间 */
    lv_label_set_text(s_lbl_runtime, "00:00:00"); /* lv_label_set_text：设置 label 文本 */
    lv_obj_set_style_text_font(s_lbl_runtime, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(s_lbl_runtime, lv_color_hex(0x4A4F58), 0);
    lv_obj_set_clickable(s_lbl_runtime, false);

    /* 中间：页码指示点 */
    create_dots(bar);

    lv_obj_t *right = lv_obj_create(bar); /* 右侧组：WiFi + 电池横向排 */
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT); /* 尺寸随内容自适应 */
    lv_obj_set_clickable(right, false);
    lv_obj_set_scrollable(right, false);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 6, 0);

    /* WiFi 信号条（v0.6.0 需求 4）：4 格自绘小条，随连接状态/信号强度点亮。
     * 放在 flex 横排的右侧组里，底部对齐（而右侧组是居中对齐）——故再套一层
     * 固定尺寸容器，让条组在 22px 高的状态栏内垂直居中。 */
    lv_obj_t *wifi = lv_obj_create(right);
    lv_obj_remove_style_all(wifi);
    lv_obj_set_size(wifi, WIFI_BAR_COUNT * WIFI_BAR_W + (WIFI_BAR_COUNT - 1) * WIFI_BAR_GAP, 12);
    lv_obj_set_clickable(wifi, false);
    lv_obj_set_scrollable(wifi, false);
    lv_obj_set_flex_flow(wifi, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wifi, WIFI_BAR_GAP, 0);
    for (int i = 0; i < WIFI_BAR_COUNT; i++) {
        lv_obj_t *b = lv_obj_create(wifi);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, WIFI_BAR_W, s_wifi_bar_h[i]);
        lv_obj_set_style_bg_color(b, WIFI_COLOR_OFF, 0); /* 初始灰：未连接 */
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_clickable(b, false);
        s_wifi_bars[i] = b;
    }

    s_lbl_bat = lv_label_create(right);
    lv_label_set_text(s_lbl_bat, LV_SYMBOL_BATTERY_FULL " --%"); /* 图标字符与文本可直接拼接 */
    lv_obj_set_style_text_font(s_lbl_bat, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
    lv_obj_set_style_text_color(s_lbl_bat, BAT_COLOR_NONE, 0);
    lv_obj_set_clickable(s_lbl_bat, false);
}

/**
 * @brief 刷新状态栏 WiFi 信号条（v0.6.0 需求 4）
 *
 * 形态由 wifi_manager 的**实时状态**决定（不再是一个永远不变的灰图标）：
 *   · 已连接   → 按 RSSI 点亮 1~4 格：3~4 格绿色，1~2 格黄色（弱信号警示）；
 *   · 配网模式 → 4 格全蓝（表示"本机正在提供热点"，与"连上路由器"区分）；
 *   · 连接中   → 1 格黄色（表示在尝试，不冒充已连接）；
 *   · 其余（未启动 / 失败 / 无凭据） → 全灰。
 *
 * 只读 wifi_manager 的快照接口（非阻塞），可安全地在 5Hz 状态栏刷新里调用。
 */
static void refresh_wifi_bars(void)
{
    if (s_wifi_bars[0] == NULL) {
        return; /* 状态栏尚未建立 */
    }
    const wifi_manager_state_t st = wifi_manager_get_state();
    int lit = 0;
    lv_color_t col = WIFI_COLOR_OFF;

    if (st == WIFI_MANAGER_STATE_CONNECTED) {
        int8_t rssi = 0;
        if (wifi_manager_get_rssi(&rssi) == ESP_OK) {
            lit = wifi_manager_rssi_level(rssi);
            col = (lit <= 2) ? WIFI_COLOR_WEAK : WIFI_COLOR_ON;
        } else {
            lit = 1;
            col = WIFI_COLOR_ON;
        }
    } else if (st == WIFI_MANAGER_STATE_AP_MODE) {
        lit = WIFI_BAR_COUNT;
        col = WIFI_COLOR_AP;
    } else if (st == WIFI_MANAGER_STATE_CONNECTING) {
        lit = 1;
        col = WIFI_COLOR_WEAK;
    }

    for (int i = 0; i < WIFI_BAR_COUNT; i++) {
        lv_obj_set_style_bg_color(s_wifi_bars[i], (i < lit) ? col : WIFI_COLOR_OFF, 0);
    }
}

/**
 * @brief 用最新快照刷新状态栏的运行时间与电池显示
 *
 * @param snap 数据快照（运行时间/电池电压都从中取）
 */
static void refresh_status(const data_store_snapshot_t *snap)
{
    uint32_t total = snap->runtime_s;
    char rt[16];
    snprintf(rt, sizeof(rt), "%02u:%02u:%02u", (unsigned)(total / 3600),
             (unsigned)((total / 60) % 60), (unsigned)(total % 60));
    lv_label_set_text(s_lbl_runtime, rt); /* 更新运行时间文本（HH:MM:SS） */

    bool has = (snap->battery_mv > 0);
    uint8_t pct = battery_percent(snap->battery_mv);
    lv_color_t col = battery_color(pct, has);
    /* 电量档位换图标：LVGL 内置电池符号从 EMPTY 到 FULL 共 5 档 */
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
        snprintf(bat, sizeof(bat), "%s --%%", icon); /* 无数据时百分比画横线占位 */
    }
    lv_label_set_text(s_lbl_bat, bat);
    lv_obj_set_style_text_color(s_lbl_bat, col, 0); /* 电量颜色随百分比变（绿/黄/红） */

    /* v0.6.0：WiFi 信号条与电量同一节拍刷新（5Hz），状态变化即时可见 */
    refresh_wifi_bars();
}

/**
 * @brief 按当前页更新指示点样式：当前页拉长变蓝，其余缩为灰圆点
 */
static void update_dots(void)
{
    for (int i = 0; i < UI_PAGE_MAX; i++) {
        bool cur = (i == (int)s_current);
        lv_obj_set_width(s_dots[i], cur ? DOT_W_CUR : DOT_SIZE);
        lv_obj_set_height(s_dots[i], cur ? DOT_H_CUR : DOT_SIZE);
        lv_obj_set_style_bg_color(s_dots[i],
                                  cur ? lv_color_hex(0x0066CC) : lv_color_hex(0xC2C7CF), 0);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0); /* LV_RADIUS_CIRCLE：全圆角 */
    }
}

/* ---- 垂直/水平滑动识别（indev 级 PRESSED/RELEASED 自测向量）----
 * LVGL 的 LV_EVENT_GESTURE 不可靠：indev_gesture() 里 scroll_obj 非空即
 * return（被 tileview 滚动抑制），且要求瞬时速度 ≥ gesture_min_velocity，
 * 慢速上下滑永远收不到——实机"上滑/下滑无反应"根因。
 * 改为按下记起点、抬起算总位移：垂直为主 → 报上下滑（快速菜单）；
 * 水平为主 → 交给 tileview 自身翻页（已取消端页环形翻页，首末页到头即止）。
 * 用总位移而非 LV_EVENT_SCROLL 内 hack——vect 是单帧增量，抖动/弹性期符号
 * 随机，判方向不可靠。 */
#define SWIPE_MIN_DIST 30   /**< 垂直判定最小位移（px） */
#define SWIPE_HS_MIN_DIST 60 /**< "显著水平位移"判定阈值（px，≈20% 页宽），用于断触续接判据③ */
/* ---- 断触续接吞没（防"一次长滑被拆成两段 → 连翻两页"）----
 * 实机问题：长距离快速滑动时电容触摸偶发断触（触点丢失又立即重现），
 * LVGL 把它切成 PRESSED-RELEASED-PRESSED-RELEASED 两段独立手势；
 * 两段各滑过半页宽 → tileview 吸附动画各翻一页 → 一次滑动连翻两页。
 *
 * 判据（三条同时满足才认定"断触续接"，避免误伤"快速连翻两次"）：
 *   ① 两次触摸间隔 < SWIPE_MERGE_MS——断触的物理间隔通常只有几十毫秒，
 *      而"抬手→移动→再按"完成第二次独立滑动很难压进 250ms；
 *   ② 续接按下点距上段抬起点 < SWIPE_MERGE_DIST——断触时手指停在原地
 *      附近，独立滑动的第二次起手点通常明显不同；
 *   ③ 上一段是显著水平滑动（抬手位移过阈值），点击/微抖不触发吞没。
 *
 * 处理：吞掉续接段的水平拖动——冻结 tileview（scrollable off）让上段的
 * 吸附动画跑完、停在目标页，续接段拖不动就不会再翻页；抬手恢复滚动。
 * 垂直菜单判定在续接段仍保留（吞的只是水平拖动/翻页）。 */
#define SWIPE_MERGE_MS   250 /**< 断触续接判定：两段触摸最大间隔（ms） */
#define SWIPE_MERGE_DIST 35  /**< 断触续接判定：两段起止点最大距离（px） */
static lv_point_t s_swipe_start; /**< 本次手势的按下起点（PRESSED 时记录） */
static bool s_swipe_valid = false; /**< 起点有效标志（防 RELEASED 无对应 PRESSED 时误判） */
/** 上段手势抬手时刻（lv_tick 毫秒，0=无上段）。
 *  lv_tick_get() 在真机 = 物理时间（FreeRTOS tick）；在模拟器 = 虚拟时钟
 *  （按场景步骤推进），但两次独立手势的间隔都 >250ms，不影响判定正确性。 */
static uint32_t s_last_release_ms = 0;
static lv_point_t s_last_release_pt;   /**< 上段手势抬手坐标（续接距离判定用） */
static bool s_last_release_hscroll = false; /**< 上段是否有显著水平位移（③） */
static bool s_swallow = false; /**< 本次手势为断触续接段，水平拖动被吞 */

static void goto_page(ui_page_t page); /* 前向声明 */

/**
 * @brief indev（输入设备）级滑动识别：垂直滑 → 快速菜单；水平滑交给 tileview 翻页
 *
 * 挂在触摸屏 indev 的 PRESSED/RELEASED 事件上。方向判定只用**总位移**
 * （RELEASED 坐标 − PRESSED 坐标），不用 LV_EVENT_SCROLL 的单帧增量——
 * 后者与 tileview 弹性回弹冲突（详见文件头"核心机制"）。
 *
 * @param e LVGL 事件对象（target 是 lv_indev_t*，不是被按下的控件！）
 */
static void indev_swipe_event_cb(lv_event_t *e)
{
    /* ⚠ indev 事件的 target 就是 lv_indev_t*（lv_indev_send_event →
     * lv_event_push_and_send(&indev->event_list, code, indev, param)），
     * param 反而是被按下的对象。取错来源会把对象指针当 indev 解析、
     * lv_indev_get_type() 读垃圾值提前 return，整个滑动识别静默失效
     * （2026-10 实机"上滑下滑无反应 + 环形翻页失效"根因）。 */
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_target(e);
    /* lv_indev_get_type：查询输入设备类型；只对 POINTER（触摸/鼠标）做滑动识别 */
    if (indev == NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(e); /* 取事件类型（PRESSED/RELEASED） */
    if (code == LV_EVENT_PRESSED) {
        /* lv_indev_get_point：读当前触点坐标（按下瞬间记为起点） */
        lv_indev_get_point(indev, &s_swipe_start);
        s_swipe_valid = true;

        /* ---- 断触续接判定（见上方 SWIPE_MERGE_* 注释）----
         * 三条判据同时满足才吞掉本段的水平拖动。 */
        s_swallow = false;
        if (s_last_release_ms != 0 && s_tileview != NULL) {
            uint32_t gap = lv_tick_get() - s_last_release_ms; /* 与上段抬手间隔（ms） */
            int32_t ddx = s_swipe_start.x - s_last_release_pt.x;
            int32_t ddy = s_swipe_start.y - s_last_release_pt.y;
            if (gap < SWIPE_MERGE_MS && LV_ABS(ddx) < SWIPE_MERGE_DIST &&
                LV_ABS(ddy) < SWIPE_MERGE_DIST && s_last_release_hscroll) {
                s_swallow = true;
                /* 冻结 tileview：本段拖不动它，上段的吸附动画得以跑完停在目标页。
                 * lv_obj_set_scrollable：关闭对象滚动能力（触摸拖动不再移动内容）。 */
                lv_obj_set_scrollable(s_tileview, false);
            }
        }
    } else if (code == LV_EVENT_RELEASED && s_swipe_valid) {
        s_swipe_valid = false;
        if (s_swallow) {
            /* 断触续接段：吞掉水平拖动/翻页，恢复滚动供后续正常手势使用 */
            s_swallow = false;
            if (s_tileview != NULL) {
                lv_obj_set_scrollable(s_tileview, true);
            }
            /* 本段抬手点仍记入"上段"链——手指可能断触多次，链式续接都要吞 */
            lv_indev_get_point(indev, &s_last_release_pt);
            s_last_release_ms = lv_tick_get();
            s_last_release_hscroll = true; /* 延续上段判定，保持吞没链 */
            return;
        }
        lv_point_t p;
        lv_indev_get_point(indev, &p); /* 抬起点坐标 */
        /* 总位移 = 抬起点 − 起点：不受松手后弹性回弹影响（回弹不改这两个点） */
        int32_t dx = p.x - s_swipe_start.x;
        int32_t dy = p.y - s_swipe_start.y;

        /* 记录本段抬手信息，供下一次按下做"断触续接"判定。
         * s_last_release_hscroll 要求水平位移过阈值：点击/微抖不算，不吞后段。 */
        s_last_release_ms = lv_tick_get();
        s_last_release_pt = p;
        s_last_release_hscroll =
            (LV_ABS(dx) >= SWIPE_HS_MIN_DIST && LV_ABS(dx) > LV_ABS(dy));

        /* 垂直为主：上下滑 → 快速菜单（测量页/波形页都支持）。
         * 菜单开/关走各自的动画，不需要延迟（不涉及 tileview 滚动冲突）。
         * LV_ABS：取绝对值；判定"垂直为主"要求垂直位移既过阈值又大于水平位移。 */
        if (LV_ABS(dy) >= SWIPE_MIN_DIST && LV_ABS(dy) > LV_ABS(dx)) {
            lv_dir_t dir = (dy < 0) ? LV_DIR_TOP : LV_DIR_BOTTOM; /* 屏幕坐标 y 向下为正 */
            if (s_current == UI_PAGE_MEASURE) {
                screen_measure_swipe_cb(dir);
            } else if (s_current == UI_PAGE_WAVE) {
                screen_wave_swipe_cb(dir);
            }
            return;
        }

        /* 水平为主：不做任何额外处理——完全交给 tileview 自身滚动手势翻页。
         * 【取消环形翻页，2026-10 需求调整】原先"端页朝外滑 → 环到另一端页"的
         * 逻辑（wrap_goto_async）已移除：超出一页范围的翻页对用户是意外行为，
         * 首末页滑到头即止（tileview 弹性回弹）更符合直觉。 */
    }
}

/**
 * @brief tileview 滚动跟踪当前页（仅跟踪，不做环回）
 *
 * 挂在 tileview 的 LV_EVENT_SCROLL 上，滚动过程中实时算出"哪一页占屏幕
 * 多数"并更新 s_current 与指示点，让 ui.c 能按当前页决定刷新哪一屏。
 *
 * @param e LVGL 事件对象（target 是 tileview）
 */
static void scroll_event_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    int32_t w = lv_obj_get_width(tv);
    /* lv_obj_get_scroll_x：LVGL 9 API，取对象当前横向滚动偏移（px）。
     * tileview 三页横向并排，偏移/页宽 = 当前页序号。 */
    int32_t x = lv_obj_get_scroll_x(tv);
    if (w <= 0) {
        return;
    }
    /* +w/2 再整除 = 四舍五入到最近的页（滚动过半就算下一页） */
    int32_t idx = (x + w / 2) / w;
    if (idx < 0) {
        idx = 0; /* 弹性回弹期间偏移可能瞬时越界，钳到合法范围 */
    } else if (idx >= UI_PAGE_MAX) {
        idx = UI_PAGE_MAX - 1;
    }
    ui_page_t page = (ui_page_t)idx;
    if (page == s_current) {
        return; /* 页没变，不重复刷指示点 */
    }
    s_current = page;
    update_dots();
}

/**
 * @brief 按键翻页入口：滚动 tileview 到目标页（带动画）
 *
 * @param page 目标页码（越界则忽略）
 */
static void goto_page(ui_page_t page)
{
    if (page < 0 || page >= UI_PAGE_MAX) {
        return;
    }
    /* lv_obj_scroll_to_view：把某个子对象滚动到可见位置（这里是滚到目标 tile），
     * LV_ANIM_ON = 带平滑过渡动画，视觉上就是"翻页"。 */
    lv_obj_scroll_to_view(s_tiles[page], LV_ANIM_ON);
}

/**
 * @brief 构建 tileview + 三页内容（首次构建与旋转重建共用）
 *
 * 三页里大量元素使用按当前分辨率计算的固定像素坐标（测量页四行数据条、
 * 波形页图表等），LVGL 改分辨率只会缩放 pct 对象，固定坐标不会自动重排，
 * 因此旋转（RESOLUTION_CHANGED）后直接按新分辨率**重建整组页面**最可靠。
 * 各 screen 的 create() 都会读取当前显示分辨率决定几何，横屏构建结果与
 * 历史布局逐像素一致。
 *
 * @param keep 重建后要停留的页面（旋转前的当前页）
 */
static void build_pages(ui_page_t keep)
{
    lv_obj_t *scr = lv_screen_active(); /* 当前活动屏（所有顶层对象的根） */

    /* lv_tileview_create：LVGL 的"磁贴视图"——一组整页子对象，可滑动切换。
     * 本项目用它承载三页横滑，比手写 scroll 容器省掉大量分页逻辑。 */
    s_tileview = lv_tileview_create(scr);
    lv_obj_set_size(s_tileview, LV_PCT(100), LV_PCT(100)); /* 铺满整屏 */
    lv_obj_set_pos(s_tileview, 0, 0);
    lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF); /* 隐藏滚动条 */
    /* LV_SCROLL_SNAP_CENTER：松手后自动吸附到最近的整页（翻页"咔哒"归位的关键） */
    lv_obj_set_scroll_snap_x(s_tileview, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scroll_dir(s_tileview, LV_DIR_HOR); /* 只允许横向滚动 */
    /* 挂滚动回调跟踪当前页（LV_EVENT_SCROLL：滚动过程中每帧触发） */
    lv_obj_add_event_cb(s_tileview, scroll_event_cb, LV_EVENT_SCROLL, NULL);

    /* lv_tileview_add_tile：往 tileview 加一页；参数（x, y）是页在磁贴网格中
     * 的坐标，LV_DIR_HOR 表示该页允许横向滑出到相邻页。三页横排：(0,0)(1,0)(2,0)。 */
    s_tiles[UI_PAGE_MEASURE] =
        lv_tileview_add_tile(s_tileview, UI_PAGE_MEASURE, 0, LV_DIR_HOR);
    s_tiles[UI_PAGE_WAVE] =
        lv_tileview_add_tile(s_tileview, UI_PAGE_WAVE, 0, LV_DIR_HOR);
    s_tiles[UI_PAGE_SETTINGS] =
        lv_tileview_add_tile(s_tileview, UI_PAGE_SETTINGS, 0, LV_DIR_HOR);

    /* create 顺序即 z 序；各 create 会重置自身的浮层/菜单状态，
     * screen_wave 的底部滑板挂在 screen 层，create 内部负责清理旧实例。 */
    screen_measure_create(s_tiles[UI_PAGE_MEASURE]);
    screen_wave_create(s_tiles[UI_PAGE_WAVE]);
    screen_settings_create(s_tiles[UI_PAGE_SETTINGS]);

    s_current = keep;
    update_dots();
    /* 【防御】断触吞没状态与"上段手势"记录是跨手势/跨重建的 static，重建后
     * 必须归零：否则残留的 s_swallow/s_last_release_hscroll 会让下一个正常
     * 手势被误判成"断触续接"而冻结 tileview、吞掉翻页（连翻/卡死的隐患源）。 */
    s_swallow = false;
    s_last_release_hscroll = false;
    s_last_release_ms = 0;
    /* 无动画滚到目标页：旋转后直接呈现该页，不让用户看到甩动过程。
     * lv_obj_update_layout：强制立即重算布局（否则刚建好的 tile 位置可能
     * 还没排好，scroll_to_view 会滚到错误位置）。 */
    lv_obj_update_layout(s_tileview);
    lv_obj_scroll_to_view(s_tiles[keep], LV_ANIM_OFF); /* LV_ANIM_OFF：不带动画，直接就位 */

    /* 新建的 tileview z 序在状态栏之上会把白底状态栏整个盖掉，
     * 必须把屏幕级浮层重新置顶（首次构建时 s_topbar 尚未创建）。
     * lv_obj_move_foreground：把对象提到同父兄弟的最上层（z 序置顶）。 */
    if (s_topbar != NULL) {
        lv_obj_move_foreground(s_topbar);
    }
}

/**
 * @brief 旋转重建的下一帧回调（避免在事件派发途中删除对象树）
 *
 * @param arg 未使用
 */
static void rebuild_pages_async_cb(void *arg)
{
    (void)arg;
    if (s_tileview == NULL) {
        return;
    }
    ui_page_t keep = s_current; /* 记住旋转前所在页，重建后回到同一页 */
    /* lv_obj_delete：删除对象及其全部子孙。三页内容随删；状态栏/圆点是
     * screen 的独立子对象，不挂在 tileview 下，因此保留。 */
    lv_obj_delete(s_tileview);
    s_tileview = NULL;
    for (int i = 0; i < UI_PAGE_MAX; i++) {
        s_tiles[i] = NULL; /* 悬空指针清零，防止误用已释放对象 */
    }
    build_pages(keep);
}

/**
 * @brief 显示分辨率变化（旋转）后按新分辨率重建三页
 *
 * 背景：tileview 自身是 pct 尺寸会跟着分辨率缩放，但页内按 284×240
 * 定位的数据条/图表不会重排（竖屏时右侧被裁、下方留大片空白）。
 * 延迟到下一帧重建：事件回调正处于 LVGL 派发栈中，当场删建对象树不安全。
 *
 * @param e LVGL 事件对象（未使用）
 */
static void disp_resolution_changed_cb(lv_event_t *e)
{
    (void)e;
    /* lv_async_call：把重建排到下一轮 LVGL timer 执行，避开当前事件派发栈 */
    lv_async_call(rebuild_pages_async_cb, NULL);
}

/**
 * @brief 在当前活动屏上构建三页导航（启动画面销毁后由 ui.c 调用）
 *
 * 顺序：清屏 → 建三页 tileview → 建共享状态栏 → 注册滑动/旋转回调。
 * 重建（旋转）时不会走本函数，而是走 rebuild_pages_async_cb。
 */
void ui_nav_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr); /* lv_obj_clean：删除屏上所有子对象（清掉启动画面残留） */
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false); /* 屏幕本身不参与滚动，滚动全归 tileview */

    build_pages(UI_PAGE_MEASURE); /* 首次构建从测量页进入 */

    /* 状态栏是 screen 级独立浮层（不随三页重建），pct 宽，旋转后自适应 */
    create_topbar(scr);
    update_dots();

    /* indev 级滑动识别（垂直=快速菜单 / 水平=交给 tileview 自身翻页）。
     * indev 生命周期与三页无关，只注册一次（s_swipe_hooked 保证旋转重建不重复挂）。 */
    static bool s_swipe_hooked = false;
    if (!s_swipe_hooked) {
        /* lv_indev_get_next：遍历 LVGL 的输入设备链表（传 NULL 从头开始，
         * 返回 NULL 表示遍历结束）——从中找出触摸屏这一路 POINTER 设备。 */
        lv_indev_t *indev = NULL;
        while ((indev = lv_indev_get_next(indev)) != NULL) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
                /* lv_indev_add_event_cb：给输入设备挂事件回调（区别于 lv_obj_add_event_cb
                 * 是给控件挂）；按下/抬起各挂一次，共用同一个回调。 */
                lv_indev_add_event_cb(indev, indev_swipe_event_cb, LV_EVENT_PRESSED, NULL);
                lv_indev_add_event_cb(indev, indev_swipe_event_cb, LV_EVENT_RELEASED, NULL);
                s_swipe_hooked = true;
            }
        }
    }

    /* 旋转（分辨率变化）后按新分辨率重建三页：display 生命周期与三页无关，
     * 此回调只注册一次（重建的是 tileview，不是 display）。 */
    /* lv_display_get_default：取默认显示设备（本项目只有一块屏） */
    lv_display_t *disp = lv_display_get_default();
    if (disp != NULL) {
        /* lv_display_add_event_cb：给显示设备挂事件回调；
         * LV_EVENT_RESOLUTION_CHANGED 在分辨率/方向变化时触发（旋转屏幕）。 */
        lv_display_add_event_cb(disp, disp_resolution_changed_cb, LV_EVENT_RESOLUTION_CHANGED,
                                NULL);
    }
}

/**
 * @brief 仅刷新共享状态栏（运行时间/页码点/电量），任何页面都可调
 *
 * @param snap 数据快照（取运行时间与电池电压）
 */
void ui_nav_refresh_statusbar(const data_store_snapshot_t *snap)
{
    /* 状态栏（时间/电量）三页都要 */
    refresh_status(snap);
    update_dots();
}

/**
 * @brief 查询当前所在页（供 ui.c 决定每页的刷新节奏）
 *
 * @return 当前页码（ui_page_t）
 */
ui_page_t ui_nav_current_page(void)
{
    return s_current;
}

/**
 * @brief 周期刷新：共享状态栏 + 当前页面内容（模拟器/整体刷新场景使用）
 *
 * 真机 ui.c 已按页面拆分为独立刷新节奏，不再统一走本函数。
 *
 * @param snap 数据快照
 */
void ui_nav_refresh(const data_store_snapshot_t *snap)
{
    ui_nav_refresh_statusbar(snap);

    /* 页面内容只刷当前可见页（见文件头"性能约定"）。
     * switch 而不是三页都刷：波形页 chart 重绘很贵，刷不可见页纯浪费。 */
    switch (s_current) {
    case UI_PAGE_MEASURE:
        screen_measure_refresh(snap);
        break;
    case UI_PAGE_WAVE:
        screen_wave_refresh();
        break;
    case UI_PAGE_SETTINGS:
        screen_settings_refresh(snap);
        break;
    default:
        break;
    }
}

/**
 * @brief 处理一个实体按键事件（UI 任务从输入队列取出后调用，已在 LVGL 锁内）
 *
 * 路由优先级：若快速菜单开着，按键先给菜单消费；否则按"单击=翻页、
 * 长按=开菜单/回首页"的规则走三页导航。
 *
 * @param id 按键编号（BUTTON_ID_BOOT=K1 / BUTTON_ID_USER=K2）
 * @param ev 按键事件类型（单击/双击/长按等）
 */
void ui_nav_handle_key(button_id_t id, button_event_t ev)
{
    /* 被接受的按键都给一声短鸣（操作反馈） */
    bool handled = false;

    /* 菜单优先：菜单/子页开着时按键全归它们（返回 true 表示已被消费）。
     * 顺序：设置页二级页/弹窗 → 测量页菜单 → 波形页菜单 → 三页导航。
     * 【P0-2 修复】设置页二级页与弹窗此前完全没有按键路径（只能触屏），
     * 现在按键优先交给 screen_settings_key() 的焦点体系处理。 */
    if (screen_settings_subpage_is_open() && screen_settings_key(id, ev)) {
        handled = true;
    } else if (screen_measure_menu_is_open() && screen_measure_menu_key(id, ev)) {
        handled = true;
    } else if (screen_wave_menu_is_open() && screen_wave_menu_key(id, ev)) {
        handled = true;
    } else if (ev == BUTTON_EVENT_SINGLE_CLICK) {
        /* 按键翻页（线性，不环回）：K1=上一页、K2=下一页，到头停止。
         * 【取消循环切换，2026-10 需求调整】原用取模实现三页环形
         * （(s_current ± 1 + MAX) % MAX），现改为直加减并在边界判断——
         * 到首/末页再按不再跳回另一端（用户期望"到头即止"）。
         * 到头时不算 handled，避免"无动作却响一声"的困惑反馈。 */
        if (id == BUTTON_ID_BOOT) {
            if (s_current > UI_PAGE_MEASURE) {
                goto_page((ui_page_t)(s_current - 1));
                handled = true;
            }
        } else {
            if ((int)s_current < (UI_PAGE_MAX - 1)) {
                goto_page((ui_page_t)(s_current + 1));
                handled = true;
            }
        }
    } else if (ev == BUTTON_EVENT_LONG_PRESS_START) {
        /* 长按 K2=开本页快速菜单（测量/波形页）；设置页长按 K1=直接回测量页 */
        if (s_current == UI_PAGE_MEASURE && id == BUTTON_ID_USER) {
            screen_measure_open_menu();
            handled = true;
        } else if (s_current == UI_PAGE_WAVE && id == BUTTON_ID_USER) {
            screen_wave_open_menu();
            handled = true;
        } else if (s_current == UI_PAGE_SETTINGS && id == BUTTON_ID_BOOT) {
            goto_page(UI_PAGE_MEASURE);
            handled = true;
        }
    }

    if (handled) {
        /* 单击与长按各给一次反馈；长按释放不重复响。
         * buzzer_play：蜂鸣驱动，播放一声短促"咔"（可在设置页关闭）。 */
        (void)buzzer_play(BUZZER_TONE_CLICK);
    }
}
