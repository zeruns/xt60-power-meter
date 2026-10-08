/**
 * @file main.c
 * @brief 应用入口：只做初始化编排与启动顺序控制，不承载业务逻辑
 *
 * 启动顺序（每一步失败都会通过 ESP_ERROR_CHECK 中止并复位）：
 *   板级初始化 → 指示灯/状态机 → 外设与服务 → 创建任务 → 进入正常运行态
 */
#include "app_tasks.h"
#include "board.h"
#include "status_led.h"
#include "system_state.h"

#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "main";

void app_main(void)
{
    /* 1. 板级初始化：必须最先执行，后续驱动都依赖它提供的引脚与总线环境 */
    ESP_ERROR_CHECK(board_init());

    /* 2. 状态指示灯与状态机：进入 INIT 状态（指示灯常亮，表示正在启动）
     *    注意顺序——指示灯必须先于状态机初始化，因为状态迁移会调用它 */
    ESP_ERROR_CHECK(status_led_init());
    ESP_ERROR_CHECK(system_state_init());

    /* 3. 外设与服务初始化在此依次加入（每接入一个模块补一行）：
     *      button_init()            -> components/drivers/button
     *      battery_adc_init()       -> components/drivers/battery_adc
     *      aht20_init()             -> components/drivers/aht20
     *      imu_sc7a20h_init()       -> components/drivers/imu_sc7a20h
     *      ads131m02_init()         -> components/drivers/ads131m02
     *      lcd_nv3030b_init()       -> components/drivers/lcd_nv3030b
     *      touch_cst816d_init()     -> components/drivers/touch_cst816d
     *      lvgl_port_init()         -> components/middleware/lvgl_port
     *      wifi_manager_start()     -> components/middleware/wifi_manager
     *    接入后建议改为「失败则 system_state_set(SYSTEM_STATE_FAULT)」而非直接中止，
     *    以便在异常状态下仍能通过状态灯快闪提示故障。 */

    /* 4. 创建应用任务（含双核亲和性绑定，见 app_tasks.h） */
    ESP_ERROR_CHECK(app_tasks_start());

    /* 5. 全部就绪，进入正常运行状态（指示灯转为 1 Hz 慢闪） */
    ESP_ERROR_CHECK(system_state_set(SYSTEM_STATE_RUNNING));

    ESP_LOGI(TAG, "启动完成");
}
