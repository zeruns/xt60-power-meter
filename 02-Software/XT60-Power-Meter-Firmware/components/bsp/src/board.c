/**
 * @file board.c
 * @brief 板级支持层实现
 *
 * 只做与具体板卡相关、且与上层业务无关的初始化工作。
 */
#include "board.h"

#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "board";

#define BOARD_NAME "XT60 Power Meter"    /**< 板卡名称 */
#define BOARD_REV  "上板 V2 / 下板 V1.0" /**< 板卡版本（以原理图版本为准） */

/**
 * @brief 板级初始化
 *
 * 应在 app_main() 中最先调用，早于任何外设驱动初始化。
 * 当前完成：打印板卡标识与芯片信息；后续在此加入共享总线（SPI/I2C）初始化。
 *
 * @return ESP_OK 成功
 */
esp_err_t board_init(void)
{
    /* 读取芯片信息，便于从日志确认实际运行环境（型号/核数/版本） */
    esp_chip_info_t chip = { 0 };
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "%s (%s)", BOARD_NAME, BOARD_REV);

    /* revision 编码为「主版本 × 100 + 次版本」，故拆成 v主.次 显示 */
    ESP_LOGI(TAG, "MCU: ESP32-S3 rev v%d.%d, %d core(s), ESP-IDF %s", chip.revision / 100,
             chip.revision % 100, chip.cores, esp_get_idf_version());

    return ESP_OK;
}
