/**
 * @file app_tasks.c
 * @brief 应用任务的创建与 CPU 亲和性绑定
 *
 * 核分配原则见 app_tasks.h：交互与通信集中在 Core 0，测量任务独占 Core 1。
 */
#include "app_tasks.h"

#include "system_state.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** 日志标签 */
static const char *TAG = "app_task";

#define SYS_MONITOR_PERIOD_MS 5000 /**< 系统监视输出周期（毫秒） */
#define SYS_MONITOR_STACK     3072 /**< 系统监视任务栈深度（字节） */
#define SYS_MONITOR_PRIO      3    /**< 系统监视任务优先级（最低，避免影响业务） */

/**
 * @brief 系统监视任务
 *
 * 周期性输出当前系统状态、所处核心与堆内存水位，便于联调阶段观察运行情况。
 * 绑定在 Core 0（交互核）。
 */
static void task_sys_monitor(void *arg)
{
    (void)arg;

    for (;;) {
        /* min_heap 反映历史最低水位，是判断内存是否紧张的关键指标 */
        ESP_LOGI(TAG, "state=%s core=%d heap=%u min_heap=%u", system_state_name(system_state_get()),
                 xPortGetCoreID(), (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());

        vTaskDelay(pdMS_TO_TICKS(SYS_MONITOR_PERIOD_MS));
    }
}

esp_err_t app_tasks_start(void)
{
    /* 使用 PinnedToCore 显式绑定到交互核，确保测量核不被监视任务打扰 */
    BaseType_t ok = xTaskCreatePinnedToCore(task_sys_monitor, "sys_monitor", SYS_MONITOR_STACK,
                                            NULL, SYS_MONITOR_PRIO, NULL, APP_CORE_UI);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 sys_monitor 任务失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
