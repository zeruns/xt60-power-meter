/**
 * @file app_tasks.c
 * @brief 应用任务的创建与运行状态巡检
 */
#include "app_tasks.h"

#include "data_store.h"
#include "power_measure.h"
#include "system_state.h"
#include "ui.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** 日志标签 */
static const char *TAG = "app_task";

/* 巡检任务的参数（周期 / 栈 / 优先级）
 *
 * ⚠ 本文件不是配置点：这三个值全部来自 Kconfig
 *   （main/Kconfig.projbuild 的 APP_SYS_MONITOR_* 项），
 *   下面直接以 CONFIG_ 宏使用，**不设本地默认值**。
 *
 * 原因：曾经用过 "#ifdef CONFIG_xxx / #else 本地默认值 / #endif" 的写法，
 *       但 Kconfig 已定义该项，sdkconfig 中必然存在对应宏，于是
 *       #else 分支成为死代码——改它无效且编译不报错。
 *
 * 修改方法：idf.py menuconfig → 组件配置 → XT60 功率计应用配置
 */

/** 巡检任务句柄，用于查询它的栈使用情况 */
static TaskHandle_t s_monitor_task = NULL;

/**
 * @brief 系统巡检任务（开发者观察窗口）
 *
 * 周期性打印运行状态、内存水位、栈水位与测量数据更新计数。
 * 联调阶段这个任务很有价值，具体表现在：
 *   · heap / min_heap 可以看出是否存在内存泄漏；
 *   · 栈水位（HighWaterMark）可以把"栈该给多大"从猜测变成实测；
 *   · update_count 能立刻判断测量核是否还在正常产出数据。
 *
 * 优先级设为最低，保证它绝不会影响业务任务。
 */
static void task_sys_monitor(void *arg)
{
    (void)arg;

    for (;;) {
        /* --- 系统状态与故障 --- */
        char fault_text[96];
        system_state_fault_to_str(system_state_get_fault_mask(), fault_text, sizeof(fault_text));

        /* --- 内存水位 ---
         * heap = 当前可用堆；min_heap = 历史最低值。
         * 若 min_heap 持续下降，说明有内存泄漏。 */
        ESP_LOGI(TAG, "状态=%s 故障=%s 核=%d 堆=%u 最低堆=%u",
                 system_state_name(system_state_get()), fault_text, xPortGetCoreID(),
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());

        /* --- 栈水位 ---
         * ⚠ 三个容易踩的坑（都是 ESP-IDF 与上游 FreeRTOS 的差异）：
         *   1. uxTaskGetStackHighWaterMark() 返回单位是「字节」，
         *      而 FreeRTOS 官方文档写的是「字」——IDF 改了语义；
         *   2. ESP-IDF 里 StackType_t 被定义成 uint8_t（portSTACK_TYPE），
         *      所以 sizeof(StackType_t) == 1，
         *      不能再按上游习惯写 stack * sizeof(StackType_t) 去凑字节；
         *   3. 同理，xTaskCreate() 的栈深度参数单位也是「字节」而不是「字」，
         *      所以下面 CONFIG_APP_SYS_MONITOR_STACK = 3072 表示 3072 字节。
         * 该值是"任务历史上栈剩余空间的最小值"，越小说明越接近栈溢出，
         * 经验上应保留 25%~50% 余量。 */
        UBaseType_t stack_free_bytes = uxTaskGetStackHighWaterMark(s_monitor_task);
        ESP_LOGI(TAG, "巡检任务栈：剩余 %u 字节 / 共 %d 字节", (unsigned)stack_free_bytes,
                 CONFIG_APP_SYS_MONITOR_STACK);

        /* --- 测量数据链路 ---
         * data_store 尚无数据时返回 ESP_ERR_INVALID_STATE，
         * 这是"测量核还没开始工作"的正常表现，不算故障。 */
        data_store_snapshot_t snapshot;
        if (data_store_get(&snapshot) == ESP_OK) {
            ESP_LOGI(TAG, "测量: %.3f V / %.3f A / %.3f W（累计更新 %u 次）", snapshot.voltage_v,
                     snapshot.current_a, snapshot.power_w, (unsigned)data_store_update_count());
        } else {
            ESP_LOGD(TAG, "测量数据尚未就绪（data_store 更新计数 %u）",
                     (unsigned)data_store_update_count());
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_SYS_MONITOR_PERIOD_MS));
    }
}

esp_err_t app_tasks_start(void)
{
    /* ---- 测量任务（最先启动，绑 Core1）----
     * 4kSPS 由 ADS131M02 DRDY 自节奏驱动；ADC 关键故障时 init 已判失败，
     * 能走到这里说明测量链路初始化通过。启动失败不阻断 UI 与巡检，
     * 但会记录错误日志（此时通常是任务内存不足，属系统级异常）。 */
    if ((system_state_get_fault_mask() & SYSTEM_FAULT_ADC) == 0) {
        esp_err_t err = power_measure_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "测量任务启动失败: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "测量前端故障，跳过测量任务启动");
    }

    /* ---- UI 任务 ----
     * 门控条件：显示链路（lcd_nv3030b + lvgl_port）必须初始化成功。
     * 走到这里说明启动表与硬件自检都已完成（见 app_init.c），
     * 但 UI 只依赖显示链路，故仍单独检查 SYSTEM_FAULT_LCD：
     *   · lvgl_port 未初始化时调用任何 LVGL API 都是未定义行为，必须先查故障位；
     *   · 触摸故障（SYSTEM_FAULT_TOUCH）不阻止 UI 启动——界面仍可显示，只是不能触控。
     * 注意：界面本身是在 ui 任务内部构建的（不在 app_main 的栈上），原因见 ui.c。 */
    if ((system_state_get_fault_mask() & SYSTEM_FAULT_LCD) == 0) {
        esp_err_t err = ui_init();
        if (err != ESP_OK) {
            /* UI 启动失败不阻断测量等业务任务，降级继续（无界面运行） */
            ESP_LOGE(TAG, "UI 启动失败: %s -> 无界面运行", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "显示链路故障，跳过 UI 启动");
    }

    /* xTaskCreatePinnedToCore 的最后一个参数就是绑定的核编号。
     * 相比 xTaskCreate（由调度器任意分配），显式绑核才能保证
     * "测量核不被通信任务打扰"这一设计目标。 */
    BaseType_t ok =
        xTaskCreatePinnedToCore(task_sys_monitor, "sys_monitor", CONFIG_APP_SYS_MONITOR_STACK, NULL,
                                CONFIG_APP_SYS_MONITOR_PRIO, &s_monitor_task, APP_CORE_UI);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 sys_monitor 任务失败（栈 %d 字节）", CONFIG_APP_SYS_MONITOR_STACK);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "应用任务创建完成（巡检任务绑定 Core %d，周期 %d ms）", APP_CORE_UI,
             CONFIG_APP_SYS_MONITOR_PERIOD_MS);
    return ESP_OK;
}
