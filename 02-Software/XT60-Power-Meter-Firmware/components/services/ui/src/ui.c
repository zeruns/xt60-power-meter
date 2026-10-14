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
 * 本任务以 5Hz 读取刷新，不直接访问任何驱动。
 *
 * 任务约定：UI 任务绑定 Core 0（交互核），所有 LVGL 访问都在
 * lvgl_port 互斥锁保护下；按键回调运行在 esp_timer 任务，只向队列投递，
 * 由本任务消费（绝不在回调里碰 LVGL）。
 */
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

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
/* 三页 + chart + msgbox 对象层级较多，栈比 demo 阶段加大 */
#define UI_TASK_STACK_BYTES 12288
#define UI_TASK_PRIORITY 12

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
#define UI_INPUT_QUEUE_LEN 16

static bool s_ui_inited = false;

/* ============================================================
 * 临时诊断开关（复现"首页数据快速刷新后 UI 卡死"）。定位后请改回 0/删除。
 *
 * 开启后 UI 任务会每隔约 1s 自动执行一步操作序列，等价于"无人值守地
 * 反复开/关快速菜单、翻页、旋转屏幕"，从而在 PC 侧串口日志里稳定复现
 * 卡死，并借助 app_task 的"冻结诊断"行判定卡在渲染管线还是 UI 任务。
 * ============================================================ */
#define UI_SELFTEST 0

/** UI 任务心跳（主循环每轮更新）：供 app_task 判断 UI 任务是否也停摆 */
static volatile int64_t s_ui_loop_us = 0;
static volatile uint32_t s_ui_loop_count = 0;

int64_t ui_get_last_loop_us(void)
{
    return s_ui_loop_us;
}

/* ---- 显示刷新配置（测量页快速菜单可改；持久化在 settings） ---- */
static volatile uint8_t s_rate_hz = UI_REFRESH_DEFAULT_HZ;

/* 快照历史环：40Hz 采样（测量核的快速显示帧），供测量页各档位做窗口平均。
 * 单写者是测量核；这里只读，seqlock 保证帧自洽。 */
#define SNAP_HIST UI_RING_SIZE
static data_store_snapshot_t s_hist[SNAP_HIST];
static int s_hist_pos = 0;   /**< 下一个写入位置 */
static int s_hist_count = 0; /**< 已填充帧数（≤ SNAP_HIST） */
static int64_t s_last_sample_us = 0;
static int64_t s_last_measure_us = 0;
static int64_t s_last_wave_us = 0;
static int64_t s_last_bar_us = 0;
static int64_t s_last_set_us = 0;

uint8_t ui_get_display_rate_hz(void)
{
    return s_rate_hz;
}

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

/** 按键输入消息（esp_timer 回调 → 队列 → UI 任务） */
typedef struct {
    button_id_t id;
    button_event_t event;
} ui_key_msg_t;

static QueueHandle_t s_key_queue = NULL;

/**
 * @brief 按键驱动回调：运行在 esp_timer 任务，禁止碰 LVGL，只投递
 */
static void button_cb(button_id_t id, button_event_t event, void *ctx)
{
    (void)ctx;
    if (s_key_queue != NULL) {
        ui_key_msg_t msg = { .id = id, .event = event };
        /* 队列满直接丢弃新事件（不阻塞 esp_timer 任务） */
        (void)xQueueSend(s_key_queue, &msg, 0);
    }
}

/**
 * @brief 处理队列中所有待处理的按键消息（调用时持有 LVGL 锁）
 */
static void drain_key_queue(void)
{
    ui_key_msg_t msg;
    while (xQueueReceive(s_key_queue, &msg, 0) == pdTRUE) {
        ui_nav_handle_key(msg.id, msg.event);
    }
}

/**
 * @brief 启动故障弹窗"确定"按钮回调：设置应答标志，让 UI 任务继续构建主界面
 *
 * 运行在 LVGL 任务（taskLVGL）上下文，通过 user_data 拿到 UI 任务的
 * volatile bool 标志地址。UI 任务在 while 循环里轮询该标志。
 */
static void splash_error_ok_cb(lv_event_t *e)
{
    volatile bool *ack = (volatile bool *)lv_event_get_user_data(e);
    if (ack != NULL) {
        *ack = true;
    }
    /* 显式关闭 msgbox：LVGL footer 按钮的内建关闭行为在某些版本不可靠，
     * 这里双保险确保对话框消失。 */
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *o = btn;
    while (o != NULL && !lv_obj_check_type(o, &lv_msgbox_class)) {
        o = lv_obj_get_parent(o);
    }
    if (o != NULL) {
        lv_msgbox_close(o);
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
    if (settings_get(&cfg) == ESP_OK && cfg.lcd_brightness >= 10) {
        bright = cfg.lcd_brightness;
    }
    (void)lcd_nv3030b_set_backlight(bright);
    ESP_LOGI(TAG, "背光已打开（%u%%）", bright);
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
 *   只负责：① 启动画面计时与进度更新；② 每 200ms 刷新一次数据（稳定 5Hz）；
 *   ③ 消费按键队列。所有 LVGL 访问仍在 lvgl_port_lock() 保护下进行。
 */
static void task_ui(void *arg)
{
    (void)arg;

    s_key_queue = xQueueCreate(UI_INPUT_QUEUE_LEN, sizeof(ui_key_msg_t));

    /* 载入用户设置的刷新率（失败则保持默认 5Hz） */
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
    if (lvgl_port_lock(UI_BUILD_LOCK_TIMEOUT_MS)) {
        splash_build(lv_screen_active());
        splash_set_progress_text("正在初始化系统…");
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "构建启动画面时获取 LVGL 锁失败");
    }

    TickType_t last = xTaskGetTickCount();
    uint32_t elapsed_ms = 0;
    while (elapsed_ms < UI_SPLASH_HOLD_MS) {
        if (lvgl_port_lock(UI_HANDLER_WAIT_MAX_MS)) {
            uint32_t pct = elapsed_ms * 100u / UI_SPLASH_HOLD_MS;
            splash_set_progress(pct);
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
        /* 首帧真正上屏后才打开背光：避免开机先看到黑屏/局部条带（见 lcd 驱动注释） */
        if (!backlight_on && lvgl_port_flush_count() > 0) {
            apply_backlight();
            backlight_on = true;
        }
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
    uint32_t fault_mask = system_state_get_fault_mask();
    if (fault_mask != 0) {
        static volatile bool s_error_ack = false;
        s_error_ack = false;

        char fault_str[96];
        system_state_fault_to_str(fault_mask, fault_str, sizeof(fault_str));
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "以下外设初始化失败，\n对应功能将不可用：\n\n%s", fault_str);
        ESP_LOGW(TAG, "启动存在故障：%s", fault_str);

        if (lvgl_port_lock(UI_BUILD_LOCK_TIMEOUT_MS)) {
            lv_obj_t *box = lv_msgbox_create(NULL);
            lv_msgbox_add_title(box, "启动提示");
            lv_msgbox_add_text(box, msg);
            lv_obj_set_width(box, 250);
            /* 中文字体 */
            lv_obj_t *ttl = lv_msgbox_get_title(box);
            if (ttl != NULL) {
                lv_obj_set_style_text_font(ttl, lvgl_port_get_font_bold(LVGL_PORT_FONT_16), 0);
            }
            lv_obj_t *content = lv_msgbox_get_content(box);
            if (content != NULL) {
                for (uint32_t i = 0; i < lv_obj_get_child_count(content); i++) {
                    lv_obj_set_style_text_font(lv_obj_get_child(content, i),
                                               lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
                }
            }
            lv_obj_t *ok = lv_msgbox_add_footer_button(box, "确定");
            lv_obj_set_style_text_font(lv_obj_get_child(ok, 0),
                                       lvgl_port_get_font(LVGL_PORT_FONT_14), 0);
            lv_obj_center(box);
            /* 确定按钮回调：设置应答标志（运行在 LVGL 任务上下文） */
            lv_obj_add_event_cb(ok, splash_error_ok_cb, LV_EVENT_CLICKED,
                                (void *)&s_error_ack);
            lvgl_port_unlock();
        }

        /* 等待用户点"确定"：UI 任务挂起，LVGL 任务（taskLVGL）继续跑渲染
         * 与处理按钮事件。20ms 轮询，对功耗/响应无感知影响。 */
        while (!s_error_ack) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* ---- ② 构建主界面三页 ---- */
    if (lvgl_port_lock(UI_BUILD_LOCK_TIMEOUT_MS)) {
        ui_nav_build();
        lvgl_port_unlock();
        ESP_LOGI(TAG, "主界面已构建：测量 / 波形 / 设置");
    } else {
        ESP_LOGE(TAG, "构建主界面时获取 LVGL 锁失败");
        vTaskDelete(NULL);
        return;
    }

    /* 按键事件回调：此时按键驱动已在启动表中初始化 */
    esp_err_t err = button_register_callback(BUTTON_ID_BOOT, button_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "K1 回调注册失败: %s", esp_err_to_name(err));
    }
    err = button_register_callback(BUTTON_ID_USER, button_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "K2 回调注册失败: %s", esp_err_to_name(err));
    }

    /* ---- ③ 主循环：三个页面的刷新节奏**相互独立** ----
     *   · 快照环：25ms（40Hz）从 data_store 采一帧（测量核已按 40Hz 推快拍）；
     *   · 状态栏：5Hz（时间/电量/页码点），任何页面都刷；
     *   · 测量页：按用户设定刷新率（1/5/10/20/30Hz），对历史环取窗口平均；
     *   · 波形页：固定 25Hz（图面流畅，不受测量页刷新率影响）；
     *   · 设置页：5Hz（校准向导读数/电能统计等少量实时值）。
     * 数据源恒定 40Hz 快拍 + 200ms 权威块（电能/真 RMS 不受显示设置影响）。 */
    last = xTaskGetTickCount();

    for (;;) {
        /* UI 任务心跳：每轮更新，供 app_task 判断 UI 任务是否也停摆 */
        s_ui_loop_us = esp_timer_get_time();
        s_ui_loop_count++;

        if (lvgl_port_lock(UI_HANDLER_WAIT_MAX_MS)) {
            drain_key_queue();

            const int64_t now_us = esp_timer_get_time();

            /* ① 40Hz 快照环 */
            if (now_us - s_last_sample_us >= UI_SAMPLE_PERIOD_US) {
                s_last_sample_us = now_us;
                data_store_snapshot_t s0;
                if (data_store_get(&s0) == ESP_OK) {
                    s_hist[s_hist_pos] = s0;
                    s_hist_pos = (s_hist_pos + 1) % SNAP_HIST;
                    if (s_hist_count < SNAP_HIST) {
                        s_hist_count++;
                    }
                }
            }

            /* ② 状态栏 5Hz（先取最新帧，下面各页复用） */
            const bool have_data = (s_hist_count > 0);
            data_store_snapshot_t latest;
            if (have_data) {
                latest = s_hist[(s_hist_pos - 1 + SNAP_HIST) % SNAP_HIST];
            }
            if (have_data && now_us - s_last_bar_us >= UI_STATUSBAR_PERIOD_US) {
                s_last_bar_us = now_us;
                ui_nav_refresh_statusbar(&latest);
            }

            const ui_page_t page = ui_nav_current_page();

            /* ③ 测量页：设定刷新率 + 窗口平均（1s/200ms/100ms/50ms/即时） */
            if (have_data && page == UI_PAGE_MEASURE) {
                const uint8_t hz = s_rate_hz ? s_rate_hz : 5;
                if (now_us - s_last_measure_us >= 1000000 / (int64_t)hz) {
                    s_last_measure_us = now_us;
                    /* 窗口帧数：40Hz 源下 1Hz→40、5Hz→8、10Hz→4、20Hz→2、30Hz→1 */
                    int win = 40 / (hz >= 1 ? hz : 1);
                    if (win < 1) {
                        win = 1;
                    }
                    if (win > s_hist_count) {
                        win = s_hist_count;
                    }
                    data_store_snapshot_t out =
                        s_hist[(s_hist_pos - 1 + SNAP_HIST) % SNAP_HIST];
                    if (win >= 2) {
                        double sv = 0, si = 0, sp = 0;
                        for (int k = 0; k < win; k++) {
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
                    screen_measure_refresh(&out);
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

#if UI_SELFTEST
            /* ---- 临时诊断：UI 压力自测（每约 1s 一步，循环往复）----
             * 每步打印当前页/菜单状态，便于从日志还原"卡死前最后一步"。 */
            {
                static int64_t s_st_us = 0;
                static int s_step = 0;
                if (now_us - s_st_us >= 1000000) {
                    s_st_us = now_us;
                    ESP_LOGW(TAG, "[自测] step=%d 页=%d 菜单=%d", s_step, (int)ui_nav_current_page(),
                             (int)screen_measure_menu_is_open());
                    switch (s_step % 12) {
                    case 0: /* 开测量快速菜单 */
                        ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_LONG_PRESS_START);
                        break;
                    case 1: /* 焦点下移 */
                        ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_SINGLE_CLICK);
                        break;
                    case 2: /* 激活（切刷新率档） */
                        ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_DOUBLE_CLICK);
                        break;
                    case 3: /* 关菜单 */
                        ui_nav_handle_key(BUTTON_ID_BOOT, BUTTON_EVENT_LONG_PRESS_START);
                        break;
                    case 4: /* → 波形页 */
                        ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_SINGLE_CLICK);
                        break;
                    case 5: /* → 设置页 */
                        ui_nav_handle_key(BUTTON_ID_USER, BUTTON_EVENT_SINGLE_CLICK);
                        break;
                    case 6: /* 旋转 90°（竖屏，触发整树重建） */
                        lvgl_port_set_rotation_angle(90);
                        break;
                    case 7: /* 旋转回 0° */
                        lvgl_port_set_rotation_angle(0);
                        break;
                    case 8: /* ← 波形页 */
                        ui_nav_handle_key(BUTTON_ID_BOOT, BUTTON_EVENT_SINGLE_CLICK);
                        break;
                    case 9: /* ← 测量页 */
                        ui_nav_handle_key(BUTTON_ID_BOOT, BUTTON_EVENT_SINGLE_CLICK);
                        break;
                    case 10: /* 再次开菜单 */
                        screen_measure_open_menu();
                        break;
                    case 11: /* 旋转 90°（菜单开着时旋转） */
                        lvgl_port_set_rotation_angle(90);
                        break;
                    default:
                        break;
                    }
                    s_step++;
                }
            }
#endif

            lvgl_port_unlock();
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(UI_LOOP_PERIOD_MS));
    }
}

esp_err_t ui_init(void)
{
    if (s_ui_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(task_ui, UI_TASK_NAME,
                                            UI_TASK_STACK_BYTES, NULL,
                                            UI_TASK_PRIORITY, NULL,
                                            UI_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 UI 任务失败（栈 %d 字节）", UI_TASK_STACK_BYTES);
        return ESP_ERR_NO_MEM;
    }

    s_ui_inited = true;
    ESP_LOGI(TAG, "UI 任务已启动（Core %d，栈 %d 字节，界面在任务内构建）",
             UI_TASK_CORE, UI_TASK_STACK_BYTES);
    return ESP_OK;
}
