/**
 * @file main.c
 * @brief 应用入口：只负责"编排启动顺序"，不写任何业务逻辑
 *
 * 这是一个刻意保持很薄的文件。ESP-IDF 规定 app_main() 是固件的入口，
 * 如果在这里堆业务代码，很快会变成没人敢动的巨型函数。
 * 因此这里只做三件事：
 *   ① 调用 app_init_startup() 完成初始化（细节见 app_init.c）；
 *   ② 创建各应用任务（细节见 app_tasks.c）；
 *   ③ 记录启动结果。
 *
 * 特别说明：本工程刻意不使用 ESP_ERROR_CHECK。
 * 原因见 app_init.c 文件头——那会导致"某个外设坏掉 →
 * 不断复位 → 设备变砖"。这里遇到故障只是记录并降级运行。
 */
#include "app_init.h"
#include "app_tasks.h"
#include "system_state.h"

#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "main";

void app_main(void)
{
    /* ---------- ① 系统初始化（含分级容错） ---------- */
    esp_err_t err = app_init_startup();
    if (err != ESP_OK) {
        /* 走到这里说明关键模块（如测量前端）初始化失败，或关键自检项未通过。
         * 不做复位，而是让设备停在异常状态：
         *   · 状态停在 INIT（未调用 system_state_start_running()，不会推导成
         *     FAULT），指示灯按 INIT 常亮，故障细节由串口日志中的故障位图给出；
         *   · 串口有完整日志；
         *   · WiFi 与网页控制台仍会起来，可读状态/日志辅助定位；但当前
         *     web_console 未注册 OTA 路由，救回仍需 USB/串口重新烧录。 */
        char fault_text[96];
        system_state_fault_to_str(system_state_get_fault_mask(), fault_text, sizeof(fault_text));
        ESP_LOGE(TAG, "启动未完成：存在致命故障（%s），系统保持异常状态等待处理", fault_text);
    }

    /* ---------- ② 创建应用任务 ---------- */
    err = app_tasks_start();
    if (err != ESP_OK) {
        /* 任务都建不起来通常是内存不足，属于系统级严重问题，
         * 登记为 INFRA 故障（该位在关键掩码中，会把状态推到 FAULT），
         * 以便在指示灯与日志中明确体现。同样不使用 abort()，理由同上。 */
        ESP_LOGE(TAG, "创建应用任务失败: %s", esp_err_to_name(err));
        system_state_report_fault(SYSTEM_FAULT_INFRA);
    }

    /* ---------- ③ 退出 app_main ----------
     * app_main 返回后，它的任务会被删除，但 FreeRTOS 调度器继续运行，
     * 我们创建的任务会照常工作。这是 ESP-IDF 的正常用法。 */
    ESP_LOGI(TAG, "app_main 结束，系统交由 FreeRTOS 调度");
}
