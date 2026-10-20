/**
 * @file ui.c
 * @brief LVGL 界面层实现：启动画面 → 三页主界面（测量/波形/设置）+ UI 任务
 *
 * 界面组成：
 *   ① 启动画面（splash）：白底 + Bold 标题 + 加载条，停留 3 秒；
 *   ② 主界面（ui_nav）：lv_tileview 三页——测量数据 / 波形 / 设置，
 *      底部页面指示点；触摸横滑或 K1/K2 按键翻页；波形页上滑出设置滑板。
 *
 * 数据来源：middleware/data_store 快照（测量任务在 Core1 写入），
 * 本任务按页面节奏读取刷新：测量页 1/5/10/20/30Hz 可设、波形页固定 25Hz、
 * 状态栏与设置页 5Hz，不直接访问任何驱动。
 *
 * 任务约定：UI 任务绑定 Core 0（交互核），所有 LVGL 访问都在
 * lvgl_port 互斥锁保护下；按键回调运行在 esp_timer 任务，只向队列投递，
 * 由本任务消费（绝不在回调里碰 LVGL）。
 *
 * 【给初学者的导航设计导读】
 * 三页之间的循环滑动（测量页右滑跳到设置页、设置页左滑跳回测量页）由
 * ui_nav.c 实现。这里只提示一个关键点：方向判定必须在 LV_EVENT_RELEASED
 * 阶段用"总位移"（按下点→抬起点）计算，而不能在 LV_EVENT_SCROLL 里用
 * 单帧增量——原因是滑动过程中 tileview 在端页会被拖出边界，松手时 LVGL
 * 的弹性回弹（scroll elastic）让单帧增量的符号随机抖动，用单帧增量判定
 * 会出现"轻滑跳第三页、甩动跳回第一页"之类的误跳。详见 ui_nav.c 文件头。
 *
 * 本文件的启动流程（task_ui 内顺序执行）：
 *   ① 启动画面 splash（进度条 1800ms）；
 *   ①.5 若有外设初始化故障 → 弹 msgbox，等用户点"确定"才进主界面
 *        （为什么先弹再进：故障信息必须让用户看到并确认，不能一闪而过）；
 *   ② 构建主界面三页（ui_nav_build）；
 *   ③ 主循环：快照环采样 + 按页面节奏刷新 + 消费按键队列。
 */
#include <stdint.h>
#include <string.h>

#include <math.h> /* isfinite：过温告警框判断 soc_temp 是否有效（v0.6.4 需求 3） */

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h" /* v0.6.2：heap_caps_calloc 把快照环分配到 PSRAM */
#include "esp_timer.h"
#include "esp_task_wdt.h" /* 看门狗：UI 任务订阅它以便卡死时能打印调用栈（v0.6.0） */

#include "button.h"
#include "data_store.h"
#include "lcd_nv3030b.h"
#include "lvgl.h"
#include "settings.h"
#include "system_state.h"

#include "lvgl_port.h"
#include "screens/screen_measure.h"
#include "screens/screen_settings.h"
#include "screens/screen_wave.h"
#include "screens/splash.h"
#include "ui.h"
#include "ui_nav.h"

static const char *TAG = "ui";

/* 组件不得依赖 main 层的 APP_CORE_UI 宏，此处本地定义 UI 任务核绑定 */
#define UI_TASK_CORE 0
#define UI_TASK_NAME "ui"
/* 三页 + chart + msgbox + 设置二级页（含虚拟键盘/扫描列表）对象层级较多。
 * 【v0.6.0】由 12288 提到 16384：与 taskLVGL 同步（项目实测 12KB 栈在
 * 软件旋转 + 波形页渲染时余量不足 200B，设置子页构建也跑在本任务栈上，
 * 余量过低会触发栈溢出保护而卡死界面）。 */
#define UI_TASK_STACK_BYTES 16384 /**< UI 任务栈字节数（xTaskCreatePinnedToCore 以字节计） */
#define UI_TASK_PRIORITY 12       /**< UI 任务优先级：高于空闲/日志任务，保证交互跟手 */

#define UI_BUILD_LOCK_TIMEOUT_MS 5000 /**< 构建界面时获取 LVGL 锁的超时 */
#define UI_HANDLER_WAIT_MAX_MS 200    /**< 单次等待 LVGL 锁的上限 */
#define UI_LOOP_PERIOD_MS 5           /**< 主循环节拍（200Hz 检查点；30Hz 刷新档
                                       *   需要 ≤33ms 判定粒度，故收紧到 5ms） */
#define UI_SPLASH_HOLD_MS 1800        /**< 启动画面停留时长 */
#define UI_REFRESH_DEFAULT_HZ 5       /**< 测量页默认刷新率 */
#define UI_SAMPLE_PERIOD_US 25000     /**< 快照环采样周期（25ms=40Hz，与测量核
                                       *   PM_DISPLAY_SUBWINDOW 的快拍频率一致） */
#define UI_RING_SIZE 40               /**< 1 秒快照环（40 帧 @40Hz） */
#define UI_WAVE_PERIOD_US 40000       /**< 波形页固定 25Hz（图面流畅，独立于测量页） */
#define UI_STATUSBAR_PERIOD_US 200000 /**< 状态栏/设置页 5Hz */
#define UI_BACKLIGHT_PERCENT 100      /**< 背光兜底默认值（用户已保存亮度时以 NVS 值为准） */
#define UI_DIM_PERCENT 10             /**< 自动降亮后的背光百分比（v0.6.2 需求 11） */
#define UI_INPUT_QUEUE_LEN 16         /**< 按键消息队列深度（FreeRTOS 队列，满则丢弃新事件） */

static bool s_ui_inited = false; /**< ui_init 防重复初始化标志 */

/** UI 任务心跳（主循环每轮更新）：供 app_task 判断 UI 任务是否也停摆 */
static volatile int64_t s_ui_loop_us = 0;     /**< 最近一轮主循环的时间戳（esp_timer 微秒） */
static volatile uint32_t s_ui_loop_count = 0; /**< 主循环累计轮数（配合心跳判断"走没走"） */

/**
 * @brief 查询 UI 任务主循环最近一轮的时间戳（微秒）
 *
 * 由 app_task 的"冻结诊断"定期读取：若该时间长期不更新，说明 UI 任务
 * 自身卡住（多半阻塞在 LVGL 锁内），而非渲染管线（taskLVGL）的问题。
 *
 * @return 最近一轮主循环时的 esp_timer 时间（µs）
 */
int64_t ui_get_last_loop_us(void)
{
    return s_ui_loop_us;
}

/* ---- 显示刷新配置（测量页快速菜单可改；持久化在 settings） ---- */
/* 测量页刷新率（Hz）：写方是 LVGL 任务的菜单回调、读方是 UI 任务，故 volatile */
static volatile uint8_t s_rate_hz = UI_REFRESH_DEFAULT_HZ;

/* 快照历史环：40Hz 采样（测量核的快速显示帧），供测量页各档位做窗口平均。
 * 单写者是测量核；这里只读，seqlock 保证帧自洽。 */
#define SNAP_HIST UI_RING_SIZE            /**< 历史环帧数（= 1 秒 @40Hz） */
/* 【v0.6.2 内存优化】快照环从 BSS 移到 PSRAM 堆：40 × sizeof(snapshot) ≈ 4KB。
 * 它每秒被读写 40 次但每次只拷 ~96 字节（约 4KB/s），PSRAM 带宽完全够；
 * 换来的是内部 RAM 的 4KB 静态占用（.bss）改为按需堆分配。
 * 分配失败时退化为"没有历史环"：UI 直接读最新快照，只是低刷新率档位
 * 失去多帧平均的平滑效果，不影响正确性。 */
static data_store_snapshot_t *s_hist = NULL; /**< 快照环形缓冲：新帧覆盖最旧帧 */
static int s_hist_pos = 0;   /**< 下一个写入位置 */
static int s_hist_count = 0; /**< 已填充帧数（≤ SNAP_HIST） */
static int64_t s_last_sample_us = 0;   /**< 上次采样时刻（µs，下同）：40Hz 快照环节拍 */
static int64_t s_last_measure_us = 0;  /**< 上次测量页刷新时刻 */
static int64_t s_last_wave_us = 0;     /**< 上次波形页刷新时刻 */
static int64_t s_last_bar_us = 0;      /**< 上次状态栏刷新时刻 */
static int64_t s_last_set_us = 0;      /**< 上次设置页刷新时刻 */

/**
 * @brief 查询测量页当前数据刷新率（Hz）
 *
 * @return 刷新率（合法档位 1/5/10/20/30）
 */
uint8_t ui_get_display_rate_hz(void)
{
    return s_rate_hz;
}

/**
 * @brief 设置测量页数据刷新率（1/5/10/20/30 Hz），立即生效并持久化到 NVS
 *
 * 由测量页快速菜单回调（LVGL 任务上下文）调用。测量核固定以 40Hz 推快照，
 * UI 侧按所选档位对历史环取窗口平均：低档位窗口长读数稳，高档位响应快。
 * 只影响测量页；波形页固定 25Hz、设置页固定 5Hz，不受此设置影响。
 *
 * @param hz 目标刷新率（Hz）；非法值由 settings 拒绝，此处直接返回
 */
void ui_set_display_rate_hz(uint8_t hz)
{
    /* 合法性由 settings 兜底校验（1/5/10/20/30）；此处只同步运行时值。
     * 本函数由菜单回调（LVGL 任务）调用，s_rate_hz 为 volatile 供 UI 任务读取。 */
    if (settings_set_display_rate_hz(hz) != ESP_OK) {
        return;
    }
    s_rate_hz = hz;
    ESP_LOGI(TAG, "首页数据刷新率 → %u Hz", (unsigned)hz);
}

/**
 * @brief 按键输入消息结构体：按键驱动 → FreeRTOS 队列 → UI 任务 的载体
 *
 * 为什么绕一圈队列：按键驱动的回调运行在 esp_timer 任务里，而 LVGL 对象
 * 只能从持有 lvgl_port 锁的上下文访问；把事件打包成消息投给 UI 任务，
 * 由 UI 任务在锁内消费，就彻底避免了跨任务操作 LVGL。
 */
typedef struct {
    button_id_t id;       /**< 哪个按键（K1=BOOT / K2=USER） */
    button_event_t event; /**< 事件类型（单击/双击/长按等，见 button.h） */
} ui_key_msg_t;

static QueueHandle_t s_key_queue = NULL; /**< 按键消息队列（FreeRTOS queue，task_ui 内创建） */

/* ---- 自动降亮（F4，v0.6.2 语义调整） ----
 * 旧行为：无操作到点直接熄背光（set_backlight 0）。
 * 新行为（用户需求 11）：无操作到点把背光**降到 10%**（变暗但不黑屏，
 * 夜间看时间/读数仍可见），任何触摸或按键立即恢复到用户设置的亮度。
 * 档位含义同步改为 1=15s / 2=30s / 3=60s（见 settings.h）。 */
static int64_t s_last_input_us = 0; /**< 最近一次用户活动时刻（µs；按键与触摸都刷新） */
static bool s_dimmed = false;       /**< true=已降亮到 10%；触摸/按键恢复原亮度 */

static void screen_off_wake(void); /* 前向声明（定义见 apply_backlight 之后） */

/**
 * @brief 按键驱动回调：运行在 esp_timer 任务，禁止碰 LVGL，只投递
 *
 * @param id    触发的按键编号
 * @param event 按键事件类型
 * @param ctx   用户上下文（未使用）
 */
static void button_cb(button_id_t id, button_event_t event, void *ctx)
{
    (void)ctx;
    if (s_key_queue != NULL) {
        ui_key_msg_t msg = { .id = id, .event = event };
        /* xQueueSend 把消息拷入 FreeRTOS 队列；第 3 参 0 = 队列满时不等待
         * 直接失败——绝不能在 esp_timer 任务里阻塞，队列满就丢弃新事件。 */
        (void)xQueueSend(s_key_queue, &msg, 0);
    }
}

/** 当前告警弹窗对象（NULL = 未打开；v0.2.0 仪器功能批 F3） */
static lv_obj_t *s_alert_box = NULL;
/** 上次已弹过窗的告警位图：仅在上升沿弹窗，持续越限不重复弹 */
static uint32_t s_alert_shown_mask = 0;

/**
 * @brief 把对话框的标题/正文/底部按钮文字统一换成中文字体
 *
 * 【为什么必须显式设字体（v0.6.2 需求 3）】lv_msgbox_create(NULL) 把弹窗建在
 * lv_layer_top()（LVGL 顶层图层）上，它**不一定继承**当前屏幕主题里的中文字体；
 * LVGL 内置主题的默认字体是 montserrat_14，只含 ASCII、没有汉字。若不显式
 * 换成 lvgl_port 的中文字库，弹窗里的中文会逐个渲染成"方框"——2026-10 实机
 * 现象正是"预置告警弹窗的字大多是方框"。因此凡是用 lv_msgbox 显示中文处，
 * 建好后都应调用本函数（设置页另有同名思路的 style_msgbox()）。
 *
 * @param box lv_msgbox_create 返回的对话框对象
 */
static void ui_msgbox_style_cjk(lv_obj_t *box)
{
    if (box == NULL) {
        return;
    }
    /* lv_msgbox_get_title/content/footer：分别取对话框的标题栏、正文区、
     * 底部按钮区三段子对象（LVGL 9 的 msgbox 就是由这三块组成）。 */
    lv_obj_t *title = lv_msgbox_get_title(box);
    if (title != NULL) {
        /* 标题用粗体中文字号 16（与设置页标题风格一致） */
        lv_obj_set_style_text_font(title, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
    }
    lv_obj_t *content = lv_msgbox_get_content(box);
    if (content != NULL) {
        /* 正文容器里挂着若干 label，逐个换成中文字号 14 */
        for (uint32_t i = 0; i < lv_obj_get_child_count(content); i++) {
            lv_obj_set_style_text_font(lv_obj_get_child(content, i),
                                       lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
        }
    }
    lv_obj_t *footer = lv_msgbox_get_footer(box);
    if (footer != NULL) {
        /* 底部按钮区里每个按钮的第一个子对象是它的文字 label */
        for (uint32_t i = 0; i < lv_obj_get_child_count(footer); i++) {
            lv_obj_t *lbl = lv_obj_get_child(lv_obj_get_child(footer, i), 0);
            if (lbl != NULL) {
                lv_obj_set_style_text_font(lbl, lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
            }
        }
    }
}

/**
 * @brief 关闭告警弹窗（调用时持有 LVGL 锁）
 */
static void alert_box_close(void)
{
    if (s_alert_box != NULL) {
        lv_msgbox_close(s_alert_box);
        s_alert_box = NULL;
    }
}

/**
 * @brief 告警弹窗"确认"按钮回调：关闭弹窗
 */
static void alert_ok_cb(lv_event_t *e)
{
    (void)e;
    alert_box_close();
}

/**
 * @brief 处理队列中所有待处理的按键消息（调用时持有 LVGL 锁）
 *
 * 每轮主循环调用一次，把积压消息一次取空（while 循环取到队空为止），
 * 再逐条交给 ui_nav 做页面路由。告警弹窗打开时例外：按键先归弹窗。
 */
static void drain_key_queue(void)
{
    ui_key_msg_t msg;
    /* xQueueReceive：从队列取一条消息；超时 0 = 队列空立刻返回 pdFALSE，
     * 于是 while 循环在取空时退出，不会拖慢主循环节拍。 */
    while (xQueueReceive(s_key_queue, &msg, 0) == pdTRUE) {
        /* F4 自动降亮：任何按键都表明"用户在操作"——更新最后活动时刻；
         * 已降亮则立即恢复（见 screen_off_wake）。 */
        ui_note_user_activity();
        /* 告警弹窗优先于一切按键路由：弹窗期间任意按键=确认关闭
         * （与启动故障弹窗"任意按键放行"同语义——触摸排线故障时也能关） */
        if (s_alert_box != NULL) {
            alert_box_close();
            continue;
        }
        ui_nav_handle_key(msg.id, msg.event);
    }
}

/**
 * @brief 启动故障弹窗"确定"按钮回调：设置应答标志，让 UI 任务继续构建主界面
 *
 * 运行在 LVGL 任务（taskLVGL）上下文，通过 user_data 拿到 UI 任务的
 * volatile bool 标志地址。UI 任务在 while 循环里轮询该标志。
 *
 * @param e LVGL 事件对象；user_data 携带应答标志的地址，target 是被点击的按钮
 */
static void splash_error_ok_cb(lv_event_t *e)
{
    /* lv_event_get_user_data：取注册回调时传入的第 4 个参数（这里传的是
     * static 应答标志的地址），这是 LVGL 回调向外界传数据的标准做法。 */
    volatile bool *ack = (volatile bool *)lv_event_get_user_data(e);
    if (ack != NULL) {
        *ack = true;
    }
    /* 显式关闭 msgbox：LVGL footer 按钮的内建关闭行为在某些版本不可靠，
     * 这里双保险确保对话框消失。
     * lv_event_get_target：取事件作用对象（被点击的按钮）；下面向上遍历
     * parent 找到外层 msgbox——LVGL 对象树里按钮是 msgbox 的后代节点。 */
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *o = btn;
    while (o != NULL && !lv_obj_check_type(o, &lv_msgbox_class)) {
        /* lv_obj_check_type：判断对象是否属于某个类（这里判断"是不是 msgbox"）；
         * lv_obj_get_parent：取父对象，逐层上爬直到找到对话框本体。 */
        o = lv_obj_get_parent(o);
    }
    if (o != NULL) {
        lv_msgbox_close(o); /* 关闭并删除 msgbox 对象 */
    }
}

/**
 * @brief 打开背光：优先应用用户已保存的亮度（设置页可调），无效时用出厂默认
 *
 * 亮度值来自 NVS（settings 服务），因此"用户上次调好的亮度"在每次开机都生效。
 */
static void apply_backlight(void)
{
    uint8_t bright = UI_BACKLIGHT_PERCENT;
    settings_t cfg;
    /* settings_get：从 NVS（Flash 键值库）读出整套用户配置；
     * cfg.lcd_brightness < 10 视为无效值（亮度太低几乎全黑），退回默认 100%。 */
    if (settings_get(&cfg) == ESP_OK && cfg.lcd_brightness >= 10) {
        bright = cfg.lcd_brightness;
    }
    (void)lcd_nv3030b_set_backlight(bright); /* 调 LCD 驱动设置 PWM 背光占空比（%） */
    ESP_LOGI(TAG, "背光已打开（%u%%）", bright);
}

/**
 * @brief 自动降亮唤醒（F4 / v0.6.2）：已降亮则恢复用户设置的背光亮度（幂等）
 *
 * 无参数无返回值。未降亮时什么都不做，因此按键/触摸/告警弹窗等唤醒点
 * 可放心调用。本函数只碰 settings 缓存与 LCD 背光驱动，不访问 LVGL 对象树。
 */
static void screen_off_wake(void)
{
    if (!s_dimmed) {
        return;
    }
    s_dimmed = false;
    apply_backlight(); /* 恢复 cfg.lcd_brightness（settings 缓存里的用户亮度） */
}

/**
 * @brief 记录一次"用户活动"（v0.6.2 需求 11）
 *
 * 按键与**触摸**都算用户活动：旧实现只认按键，导致"手指一直在操作屏幕
 * 却被降亮"（触摸不刷新 s_last_input_us，也不唤醒）。本函数把两件事合一：
 *   ① 刷新最后活动时刻 → 自动降亮计时重新起算；
 *   ② 若已降亮 → 立即恢复用户亮度。
 *
 * 由 ui_nav 的触摸处理与 drain_key_queue 调用；线程安全前提：调用方
 * 通常持有 LVGL 锁（触摸事件本就在 LVGL 任务上下文），内部只写两个
 * 简单变量 + 调背光驱动，无 LVGL 对象访问。
 */
void ui_note_user_activity(void)
{
    s_last_input_us = esp_timer_get_time();
    screen_off_wake();
}

/**
 * @brief UI 任务：启动画面 → 主界面 → 循环驱动 LVGL
 *
 * 界面构建放在本任务（而非 ui_init()）的原因：app_main 任务栈仅 3.5KB，
 * LVGL 递归建对象极易栈溢出；本任务栈 12KB，职责边界清晰。
 *
 * 主循环节奏（重要）：
 *   LVGL 的 lv_timer_handler() 与心跳已由 ESP 官方 esp_lvgl_port 的内部任务
 *   驱动（见 middleware/lvgl_port 组件），本任务**不再自行调用** lv_timer_handler()，
 *   只负责：① 启动画面计时与进度更新；② 按页面节奏刷新数据（测量页 1~30Hz
 *   可设、波形页 25Hz、状态栏/设置页 5Hz）；③ 消费按键队列。
 *   所有 LVGL 访问仍在 lvgl_port_lock() 保护下进行。
 *
 * @param arg FreeRTOS 任务入口参数（未使用）
 */
static void task_ui(void *arg)
{
    (void)arg;

    /* xQueueCreate：创建 FreeRTOS 队列（容量 16 条按键消息）。
     * 有了队列就能削峰：UI 任务忙时消息先排队，忙完一次性 drain。 */
    s_key_queue = xQueueCreate(UI_INPUT_QUEUE_LEN, sizeof(ui_key_msg_t));

    /* v0.6.2：快照环分配到 PSRAM（见 s_hist 定义处说明）。失败则降级运行。 */
    s_hist = heap_caps_calloc(SNAP_HIST, sizeof(data_store_snapshot_t), MALLOC_CAP_SPIRAM);
    if (s_hist == NULL) {
        ESP_LOGW(TAG, "快照环 PSRAM 分配失败：退化为直接读最新快照（无多帧平均）");
    }

    /* 按键事件回调：此时按键驱动已在启动表中初始化。
     * button_register_callback：注册按键事件回调（回调运行在 esp_timer 任务，
     * 内部只往 s_key_queue 投消息，真正的处理在主循环里做）。
     * ⚠ 必须在"启动故障弹窗"之前注册：弹窗等待期间也要能收按键（P0-1 修复——
     * 之前注册在弹窗之后，触摸故障时弹窗"确定"只能靠触摸点击，按键根本不被
     * 消费，设备永久卡死在启动流程）。 */
    esp_err_t err = button_register_callback(BUTTON_ID_BOOT, button_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "K1 回调注册失败: %s", esp_err_to_name(err));
    }
    err = button_register_callback(BUTTON_ID_USER, button_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "K2 回调注册失败: %s", esp_err_to_name(err));
    }

    /* 载入用户设置的刷新率（失败则保持默认 5Hz）。
     * settings_get：从 NVS 读配置；display_rate_hz 为 0 表示未设置过。 */
    settings_t cfg0;
    if (settings_get(&cfg0) == ESP_OK && cfg0.display_rate_hz > 0) {
        s_rate_hz = cfg0.display_rate_hz;
    }
    ESP_LOGI(TAG, "首页数据刷新率：%u Hz", (unsigned)s_rate_hz);

    /* ---- ① 启动画面：边跑 handler 边计时（停留期间必须持续渲染） ----
     *
     * 进度文字按 4 个阶段切换（总时长 UI_SPLASH_HOLD_MS=1800ms）：
     *   0~30%   正在初始化系统…
     *   30~60%  正在加载字库…
     *   60~90%  正在初始化传感器…
     *   90~100% 正在准备界面…
     * 注：真实初始化在 app_init 阶段已完成，此处文字用于向用户传达启动阶段感。
     */
    bool backlight_on = false;
    /* lvgl_port_lock/unlock：获取 LVGL 互斥锁。LVGL 不是线程安全的，
     * 多任务访问其对象树前必须先拿锁（esp_lvgl_port 的标准用法）。
     * splash_build 在当前活动屏（lv_screen_active）上创建启动画面对象。 */
    if (lvgl_port_lock(UI_BUILD_LOCK_TIMEOUT_MS)) {
        splash_build(lv_screen_active());
        splash_set_progress_text("正在初始化系统…");
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "构建启动画面时获取 LVGL 锁失败");
    }

    TickType_t last = xTaskGetTickCount(); /* 记录节拍起点，供 vTaskDelayUntil 做固定周期延时 */
    uint32_t elapsed_ms = 0;
    while (elapsed_ms < UI_SPLASH_HOLD_MS) {
        if (lvgl_port_lock(UI_HANDLER_WAIT_MAX_MS)) {
            uint32_t pct = elapsed_ms * 100u / UI_SPLASH_HOLD_MS;
            splash_set_progress(pct); /* 更新进度条（0~100） */
            /* 按进度切换阶段文字（只在跨越阶段边界时更新，避免每帧刷 label） */
            if (pct < 30) {
                splash_set_progress_text("正在初始化系统…");
            } else if (pct < 60) {
                splash_set_progress_text("正在加载字库…");
            } else if (pct < 90) {
                splash_set_progress_text("正在初始化传感器…");
            } else {
                splash_set_progress_text("正在准备界面…");
            }
            lvgl_port_unlock();
        }
        /* 首帧真正上屏后才打开背光：避免开机先看到黑屏/局部条带（见 lcd 驱动注释）。
         * lvgl_port_flush_count：LVGL 每次向屏幕刷完一帧（flush）计数 +1，
         * 大于 0 表示显示内容真的已经输出到 LCD 了。 */
        if (!backlight_on && lvgl_port_flush_count() > 0) {
            apply_backlight();
            backlight_on = true;
        }
        /* vTaskDelayUntil：固定周期延时（相对"上一次唤醒时刻"），比 vTaskDelay
         * 更不容易累积漂移；pdMS_TO_TICKS 把毫秒换算成 FreeRTOS 节拍数。 */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(UI_LOOP_PERIOD_MS));
        elapsed_ms += UI_LOOP_PERIOD_MS;
    }
    /* 兜底：若因异常一直没有 flush 完成，启动画面结束后也点亮背光，避免全黑 */
    if (!backlight_on) {
        apply_backlight();
        backlight_on = true;
        ESP_LOGW(TAG, "启动画面期间未检测到上屏完成，强制打开背光");
    }

    /* ---- ①.5 启动故障提示 ----
     * 进度条跑完后检查系统故障位：若有外设初始化失败（降级），弹窗告知用户
     * 哪些功能不可用，点"确定"后再进入主界面；无故障则直接进入。
     * 关键故障（ADC/INFRA）时 app_init 已返回 ESP_FAIL，但 UI 任务仍会启动，
     * 此时弹窗同样能把故障信息呈现给用户。 */
    /* system_state_get_fault_mask：读外设初始化故障位图（每一位对应一个外设，
     * 非 0 表示至少有一个外设初始化失败、功能降级）。 */
    uint32_t fault_mask = system_state_get_fault_mask();
    if (fault_mask != 0) {
        static volatile bool s_error_ack = false; /**< 用户已点"确定"的应答标志（回调在 LVGL 任务写、UI 任务读） */
        s_error_ack = false;

        char fault_str[96];
        system_state_fault_to_str(fault_mask, fault_str, sizeof(fault_str)); /* 故障位图 → 中文描述串 */
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "以下外设初始化失败，\n对应功能将不可用：\n\n%s", fault_str);
        ESP_LOGW(TAG, "启动存在故障：%s", fault_str);

        if (lvgl_port_lock(UI_BUILD_LOCK_TIMEOUT_MS)) {
            /* lv_msgbox_create：LVGL 9 的对话框（弹窗）组件，参数为父对象
             * （传 NULL 表示挂在默认显示的活动屏上，成为顶层弹窗）。 */
            lv_obj_t *box = lv_msgbox_create(NULL);
            lv_msgbox_add_title(box, "启动提示"); /* 添加标题栏文字 */
            lv_msgbox_add_text(box, msg);         /* 添加正文文字 */
            lv_obj_set_width(box, 250);           /* 设置弹窗宽度（px） */
            /* lv_msgbox_add_footer_button：在弹窗底部加一个按钮（这里"确定"） */
            lv_obj_t *ok = lv_msgbox_add_footer_button(box, "确定");
            /* 中文字体：LVGL 默认字体不含汉字，必须显式换成 lvgl_port 的中文字库
             * （否则中文渲染成方框），详见 ui_msgbox_style_cjk 的说明 */
            ui_msgbox_style_cjk(box);
            lv_obj_center(box); /* 把弹窗摆到屏幕正中 */
            /* 确定按钮回调：设置应答标志（运行在 LVGL 任务上下文）。
             * lv_obj_add_event_cb：给对象挂事件回调；LV_EVENT_CLICKED = 被点击时触发；
             * 末参 &s_error_ack 会存进事件的 user_data，回调里再取出来。 */
            lv_obj_add_event_cb(ok, splash_error_ok_cb, LV_EVENT_CLICKED,
                                (void *)&s_error_ack);
            lvgl_port_unlock();
        }

        /* 等待用户确认：UI 任务挂起轮询，LVGL 任务（taskLVGL）继续跑渲染
         * 与处理按钮事件。
         * 【P0-1 修复】三通道确认 + 超时兜底，任一满足即放行：
         *   ① 触摸点"确定"按钮（LVGL 回调置 s_error_ack）；
         *   ② 实体按键（K1/K2 任意事件）——触摸排线故障时唯一的确认手段；
         *   ③ 30 秒超时自动放行——彻底无人操作也不至于永久卡死在启动流程
         *      （触摸故障 + 用户不在场时设备仍能进入主界面保持可用）。 */
        const int64_t ack_deadline = esp_timer_get_time() + 30 * 1000000LL;
        while (!s_error_ack) {
            /* 消费按键队列：任意按键 = 确认（启动阶段无其他按键语义） */
            ui_key_msg_t kmsg;
            if (xQueueReceive(s_key_queue, &kmsg, 0) == pdTRUE) {
                ESP_LOGI(TAG, "启动提示：用户按键确认（id=%d ev=%d）", (int)kmsg.id,
                         (int)kmsg.event);
                s_error_ack = true;
                break;
            }
            if (esp_timer_get_time() >= ack_deadline) {
                ESP_LOGW(TAG, "启动提示等待超时（30s），自动进入主界面");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        /* 收尾：超时/按键放行时弹窗还在屏上，主动关闭（触摸确认路径已由回调关闭） */
        if (lvgl_port_lock(UI_HANDLER_WAIT_MAX_MS)) {
            lv_obj_t *scr = lv_screen_active();
            for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
                lv_obj_t *o = lv_obj_get_child(scr, i);
                if (lv_obj_check_type(o, &lv_msgbox_class)) {
                    lv_msgbox_close(o);
                    break;
                }
            }
            lvgl_port_unlock();
        }
    }

    /* ---- ② 构建主界面三页 ----
     * ui_nav_build：清屏后创建 tileview 三页（测量/波形/设置）+ 共享状态栏。
     * 全程在 LVGL 锁内进行；拿不到锁说明渲染管线异常，只能删任务放弃启动界面。 */
    if (lvgl_port_lock(UI_BUILD_LOCK_TIMEOUT_MS)) {
        ui_nav_build();
        lvgl_port_unlock();
        ESP_LOGI(TAG, "主界面已构建：测量 / 波形 / 设置");
    } else {
        ESP_LOGE(TAG, "构建主界面时获取 LVGL 锁失败");
        vTaskDelete(NULL); /* 删除当前任务（参数 NULL = 删自己），不留下失控任务 */
        return;
    }

    /* ---- ③ 主循环：三个页面的刷新节奏**相互独立** ----
     *   · 快照环：25ms（40Hz）从 data_store 采一帧（测量核已按 40Hz 推快拍）；
     *   · 状态栏：5Hz（时间/电量/页码点），任何页面都刷；
     *   · 测量页：按用户设定刷新率（1/5/10/20/30Hz），对历史环取窗口平均；
     *   · 波形页：固定 25Hz（图面流畅，不受测量页刷新率影响）；
     *   · 设置页：5Hz（校准向导读数/电能统计等少量实时值）。
     * 数据源恒定 40Hz 快拍 + 200ms 权威块（电能/真 RMS 不受显示设置影响）。 */
    last = xTaskGetTickCount();
    s_last_input_us = esp_timer_get_time(); /* 输入计时起点＝进主循环时刻（防开机即降亮） */

    /* 【v0.6.0 卡死取证】把 UI 任务订阅到任务看门狗（TWDT）。
     * 背景：UI 主循环整轮持有 LVGL 锁，若某个事件回调里出现阻塞（NVS 写、
     * 等 WiFi 任务数秒），UI 任务会长时间不喂狗 → 看门狗超时（5s）时
     * ESP-IDF 会打印**卡住任务名 + 调用栈回溯**，把"卡在哪一行"落到串口，
     * 这比只看"渲染停滞"更精确。注意 CONFIG_ESP_TASK_WDT_PANIC 未开启，
     * 所以只告警、不重启，不影响现场（可安心作为常驻诊断）。
     * LVGL 任务（taskLVGL）由官方 esp_lvgl_port 创建、无法在其循环里喂狗，
     * 故不订阅它——改用 app_tasks 的 flush 增量检测（见 app_tasks.c）。 */
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "UI 任务加入看门狗失败（不影响功能，仅少一项诊断）");
    }

    for (;;) {
        /* 喂狗：本轮循环已跑完（若某回调阻塞过久，这里就喂不上 → 触发告警） */
        (void)esp_task_wdt_reset();

        /* UI 任务心跳：每轮更新，供 app_task 判断 UI 任务是否也停摆 */
        s_ui_loop_us = esp_timer_get_time(); /* 取系统启动以来的微秒时间戳（64 位单调时钟） */
        s_ui_loop_count++;

        if (lvgl_port_lock(UI_HANDLER_WAIT_MAX_MS)) {
            drain_key_queue(); /* 先处理积压按键，保证按键响应优先于数据刷新 */

            const int64_t now_us = esp_timer_get_time();

            /* ⓪ 自动降亮（F4 / v0.6.2）：距上次**用户活动**（按键或触摸）超过
             * 档位时长（1→15s 2→30s 3→60s；0=永不）→ 背光降到 10%。
             * 只在降亮瞬间写一次驱动（s_dimmed 防重复设 PWM）；
             * 触摸由 ui_note_user_activity 唤醒，按键由 drain_key_queue 唤醒。 */
            settings_t socfg;
            if (settings_get(&socfg) == ESP_OK && socfg.screen_off_s > 0 && !s_dimmed) {
                const int64_t dim_after_us = (int64_t)socfg.screen_off_s * 15LL * 1000000LL;
                if (now_us - s_last_input_us >= dim_after_us) {
                    (void)lcd_nv3030b_set_backlight(UI_DIM_PERCENT);
                    s_dimmed = true;
                    ESP_LOGI(TAG, "自动降亮：无操作超过 %us，背光降至 %u%%",
                             (unsigned)socfg.screen_off_s * 15u, (unsigned)UI_DIM_PERCENT);
                }
            }

            /* ① 40Hz 快照环：每 25ms 从 data_store 拷一帧进历史环。
             * 用"距上次采样的时间差"判节拍（而非固定次数），偶尔一次卡顿
             * 不会永久拖慢采样率。 */
            if (now_us - s_last_sample_us >= UI_SAMPLE_PERIOD_US) {
                s_last_sample_us = now_us;
                data_store_snapshot_t s0;
                if (s_hist != NULL && data_store_get(&s0) == ESP_OK) {
                    s_hist[s_hist_pos] = s0;
                    s_hist_pos = (s_hist_pos + 1) % SNAP_HIST; /* 取模实现环形覆盖 */
                    if (s_hist_count < SNAP_HIST) {
                        s_hist_count++;
                    }
                }
            }

            /* ② 状态栏 5Hz（先取最新帧，下面各页复用）。
             * (s_hist_pos - 1 + SNAP_HIST) % SNAP_HIST：环形缓冲里"最新一帧"
             * 的下标——pos 指向下一个写入位，前一格就是最新帧；加 SNAP_HIST
             * 再取模防止 pos=0 时下标为负。 */
            bool have_data = false;
            data_store_snapshot_t latest;
            if (s_hist != NULL && s_hist_count > 0) {
                latest = s_hist[(s_hist_pos - 1 + SNAP_HIST) % SNAP_HIST];
                have_data = true;
            } else if (data_store_get(&latest) == ESP_OK) {
                /* 无历史环（PSRAM 分配失败）：直接读最新快照兜底，
                 * 保证测量页/状态栏仍能刷新，只是没有多帧平均 */
                have_data = true;
            }

            /* ②b 告警上升沿检测（v0.2.0 仪器功能批 F3）：
             * alert_mask 上升沿弹窗一次；恢复到 0 时关闭在开的弹窗。
             * 每轮都查（不设周期）：弹窗是安全相关，200ms 内必须有反馈。 */
            if (have_data) {
                const uint32_t cur = latest.alert_mask;
                if (cur != 0 && cur != s_alert_shown_mask) {
                    /* 上升沿（或越限项变化）：弹窗（替换旧弹窗，不堆叠） */
                    alert_box_close();
                    char msg[96] = "";
                    if (cur & DATA_ALERT_OVP) {
                        snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "过压 %.2fV\n",
                                 (double)latest.voltage_v);
                    }
                    if (cur & DATA_ALERT_OCP) {
                        snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "过流 %.2fA\n",
                                 (double)latest.current_a);
                    }
                    if (cur & DATA_ALERT_OPP) {
                        snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "过功率 %.2fW\n",
                                 (double)latest.power_w);
                    }
                    if (cur & DATA_ALERT_OTP) {
                        /* 【v0.6.5 需求 1】过温恒开告警：判定是"两路温度任一超限"，
                         * 所以这里把**两个读数都显示出来**，让用户自己看清是哪一路
                         * 偏高（只显示一路会误导排查方向）。
                         * 无效读数（NaN）显示 "--" 占位。 */
                        char ts[16], te[16];
                        if (isfinite(latest.soc_temp_c)) {
                            snprintf(ts, sizeof(ts), "%.1f℃", (double)latest.soc_temp_c);
                        } else {
                            snprintf(ts, sizeof(ts), "--");
                        }
                        if (isfinite(latest.temperature_c)) {
                            snprintf(te, sizeof(te), "%.1f℃", (double)latest.temperature_c);
                        } else {
                            snprintf(te, sizeof(te), "--");
                        }
                        snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg),
                                 "主控 %s / 机内 %s\n请断开负载降温后再用\n", ts, te);
                    }
                    lv_obj_t *box = lv_msgbox_create(NULL);
                    /* v0.6.2 需求 3：全项目移除 U+26A0（⚠），改用全角"！"，
                     * 原因见文件头/字体工具的说明（源字体可能无该字形）。 */
                    lv_msgbox_add_title(box, "！测量告警");
                    lv_msgbox_add_text(box, msg);
                    lv_obj_t *ok = lv_msgbox_add_footer_button(box, "确认");
                    ui_msgbox_style_cjk(box); /* 显式中文字体：否则中文渲染成方框 */
                    lv_obj_add_event_cb(ok, alert_ok_cb, LV_EVENT_CLICKED, NULL);
                    s_alert_box = box;
                    /* F4：告警是安全信息，降亮中弹出也必须恢复到正常亮度让用户看到；
                     * 顺带重置输入时刻，保证弹窗至少完整展示一个降亮周期 */
                    s_last_input_us = now_us;
                    screen_off_wake();
                } else if (cur == 0 && s_alert_box != NULL) {
                    /* 恢复正常：自动关闭在开的告警弹窗 */
                    alert_box_close();
                }
                s_alert_shown_mask = cur;
            }

            if (have_data && now_us - s_last_bar_us >= UI_STATUSBAR_PERIOD_US) {
                s_last_bar_us = now_us;
                ui_nav_refresh_statusbar(&latest); /* 刷时间/电量/页码点（任何页面都刷） */
            }

            const ui_page_t page = ui_nav_current_page(); /* 查询当前页，只刷可见页 */

            /* ③ 测量页：设定刷新率 + 窗口平均（1s/200ms/100ms/50ms/即时） */
            if (have_data && page == UI_PAGE_MEASURE) {
                const uint8_t hz = s_rate_hz ? s_rate_hz : 5;
                /* 1000000/hz = 刷新周期（µs）；例如 5Hz → 每 200ms 刷一次 */
                if (now_us - s_last_measure_us >= 1000000 / (int64_t)hz) {
                    s_last_measure_us = now_us;
                    /* 窗口帧数：40Hz 源下 1Hz→40、5Hz→8、10Hz→4、20Hz→2、30Hz→1 */
                    int win = 40 / (hz >= 1 ? hz : 1);
                    if (win < 1) {
                        win = 1;
                    }
                    if (s_hist == NULL) {
                        win = 1; /* 无历史环：只能直接用最新快照，不做多帧平均 */
                    } else if (win > s_hist_count) {
                        win = s_hist_count; /* 开机初期环里帧数不足，窗口取已有帧数 */
                    }
                    /* out 初值优先取历史环最新帧；无环时用上面已读到的 latest */
                    data_store_snapshot_t out =
                        (s_hist != NULL) ? s_hist[(s_hist_pos - 1 + SNAP_HIST) % SNAP_HIST]
                                         : latest;
                    if (win >= 2 && s_hist != NULL) {
                        /* 对最近 win 帧做算术平均，抹平瞬时抖动：刷新率越低、
                         * 窗口越长、读数越稳（这也是低档位"数字不跳"的原因）。 */
                        double sv = 0, si = 0, sp = 0;
                        for (int k = 0; k < win; k++) {
                            /* 从最新帧往回数 k 格（同样用 +SNAP_HIST 防负下标） */
                            const data_store_snapshot_t *f =
                                &s_hist[(s_hist_pos - 1 - k + SNAP_HIST) % SNAP_HIST];
                            sv += f->voltage_v;
                            si += f->current_a;
                            sp += f->power_w;
                        }
                        out.voltage_v = (float)(sv / win);
                        out.current_a = (float)(si / win);
                        out.power_w = (float)(sp / win);
                    }
                    screen_measure_refresh(&out); /* 把窗口均值交给测量页刷新显示 */
                }
            } else if (have_data && page == UI_PAGE_WAVE) {
                /* ④ 波形页：固定 25Hz（与测量页刷新率互不影响） */
                if (now_us - s_last_wave_us >= UI_WAVE_PERIOD_US) {
                    s_last_wave_us = now_us;
                    screen_wave_refresh();
                }
            } else if (have_data && page == UI_PAGE_SETTINGS) {
                /* ⑤ 设置页：5Hz 实时值（校准向导/电能统计的动态标签） */
                if (now_us - s_last_set_us >= UI_STATUSBAR_PERIOD_US) {
                    s_last_set_us = now_us;
                    screen_settings_refresh(&latest);
                }
            }

            lvgl_port_unlock();
        }
        /* 固定 5ms 节拍让出 CPU（vTaskDelayUntil 不漂移）；拿锁失败的轮次
         * 也不空转，下一轮再来，保证即使 LVGL 繁忙 UI 任务心跳照常更新。 */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(UI_LOOP_PERIOD_MS));
    }
}

/**
 * @brief 启动 UI 任务（界面在其内部构建），重复调用返回错误
 *
 * 只创建任务、不直接建界面：建界面需要 12KB 栈，必须在任务自己的栈里做
 * （app_main 的栈只有约 3.5KB，放不下 LVGL 的递归建树）。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化过；ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t ui_init(void)
{
    if (s_ui_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* xTaskCreatePinnedToCoreWithCaps：创建任务并绑定 Core 0（交互核）。
     * 【v0.6.3 性能】栈由 PSRAM 改回**内部 RAM**：v0.6.1 把
     * SPIRAM_MALLOC_ALWAYSINTERNAL 降到 0 后内部 RAM 空闲约 130KB
     *（v0.6.0 当时"仅剩百字节级"的情况已不复存在），而 UI 任务栈是
     * 被高频访问的内存——每轮主循环的局部变量、LVGL 建树递归都在它上面，
     * 内部 RAM 访问比 PSRAM（走 cache）更快、更确定。
     * ⚠ UI 任务不做 ISR 相关操作（按键走事件队列），因此**内部 RAM 并非
     * 安全强制项**，只是更快；若日后内部 RAM 又紧张，改回 MALLOC_CAP_SPIRAM
     * 即可降级（功能不受影响）。测量采样任务仍必须内部 RAM 栈
     *（ISR 在其栈上运行，见架构文档）。 */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(task_ui, UI_TASK_NAME,
                                                    UI_TASK_STACK_BYTES, NULL,
                                                    UI_TASK_PRIORITY, NULL,
                                                    UI_TASK_CORE, MALLOC_CAP_INTERNAL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 UI 任务失败（栈 %d 字节）", UI_TASK_STACK_BYTES);
        return ESP_ERR_NO_MEM;
    }

    s_ui_inited = true;
    ESP_LOGI(TAG, "UI 任务已启动（Core %d，栈 %d 字节，界面在任务内构建）",
             UI_TASK_CORE, UI_TASK_STACK_BYTES);
    return ESP_OK;
}
