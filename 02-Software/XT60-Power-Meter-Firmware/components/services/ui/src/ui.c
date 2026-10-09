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

#include "button.h"
#include "data_store.h"
#include "lcd_nv3030b.h"
#include "lvgl.h"

#include "lvgl_port.h"
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
#define UI_LOOP_PERIOD_MS 20          /**< 主循环固定节拍（50Hz） */
#define UI_SPLASH_HOLD_MS 1800        /**< 启动画面停留时长 */
#define UI_REFRESH_PERIOD_MS 200      /**< 数据刷新周期（5Hz） */
#define UI_BACKLIGHT_PERCENT 100      /**< 首帧上屏后设置的背光亮度 */
#define UI_INPUT_QUEUE_LEN 16

static bool s_ui_inited = false;

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
 * @brief UI 任务：启动画面 → 主界面 → 循环驱动 LVGL
 *
 * 界面构建放在本任务（而非 ui_init()）的原因：app_main 任务栈仅 3.5KB，
 * LVGL 递归建对象极易栈溢出；本任务栈 12KB，职责边界清晰。
 *
 * 主循环节奏（重要）：
 *   固定 20ms 一拍调用 lv_timer_handler()（保证动画/输入流畅），
 *   每 10 拍（200ms）刷新一次数据 → 稳定的 5Hz 刷新率。
 *   ⚠ 不要用"lv_timer_handler() 的返回值来累加计时并延时"那种写法：
 *     返回值最大可能到数百毫秒，会把数据刷新周期拉长成几百毫秒甚至 1 秒，
 *     表现为"数据（尤其选长波形窗时）更新变慢"。用固定节拍就与它无关了。
 */
static void task_ui(void *arg)
{
    (void)arg;

    s_key_queue = xQueueCreate(UI_INPUT_QUEUE_LEN, sizeof(ui_key_msg_t));

    /* ---- ① 启动画面：边跑 handler 边计时（停留期间必须持续渲染） ---- */
    bool backlight_on = false;
    if (lvgl_port_lock(UI_BUILD_LOCK_TIMEOUT_MS)) {
        splash_build(lv_screen_active());
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "构建启动画面时获取 LVGL 锁失败");
    }

    TickType_t last = xTaskGetTickCount();
    uint32_t elapsed_ms = 0;
    while (elapsed_ms < UI_SPLASH_HOLD_MS) {
        if (lvgl_port_lock(UI_HANDLER_WAIT_MAX_MS)) {
            splash_set_progress(elapsed_ms * 100u / UI_SPLASH_HOLD_MS);
            (void)lv_timer_handler();
            lvgl_port_unlock();
        }
        /* 首帧真正上屏后才打开背光：避免开机先看到黑屏/局部条带（见 lcd 驱动注释） */
        if (!backlight_on && lvgl_port_flush_count() > 0) {
            (void)lcd_nv3030b_set_backlight(UI_BACKLIGHT_PERCENT);
            backlight_on = true;
            ESP_LOGI(TAG, "首帧已上屏，打开背光（%d%%）", UI_BACKLIGHT_PERCENT);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(UI_LOOP_PERIOD_MS));
        elapsed_ms += UI_LOOP_PERIOD_MS;
    }
    /* 兜底：若因异常一直没有 flush 完成，启动画面结束后也点亮背光，避免全黑 */
    if (!backlight_on) {
        (void)lcd_nv3030b_set_backlight(UI_BACKLIGHT_PERCENT);
        backlight_on = true;
        ESP_LOGW(TAG, "启动画面期间未检测到上屏完成，强制打开背光");
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

    /* ---- ③ 主循环：固定节拍（20ms），每 10 拍刷一次数据（5Hz） ---- */
    last = xTaskGetTickCount();
    uint32_t tick_cnt = 0;
    const uint32_t refresh_ticks = UI_REFRESH_PERIOD_MS / UI_LOOP_PERIOD_MS;

    for (;;) {
        if (lvgl_port_lock(UI_HANDLER_WAIT_MAX_MS)) {
            drain_key_queue();

            if (++tick_cnt >= refresh_ticks) {
                tick_cnt = 0;
                data_store_snapshot_t snap;
                if (data_store_get(&snap) == ESP_OK) {
                    ui_nav_refresh(&snap);
                }
            }

            (void)lv_timer_handler();
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
