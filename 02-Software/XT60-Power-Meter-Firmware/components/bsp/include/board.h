/**
 * @file board.h
 * @brief 板级支持层对外接口
 *
 * 职责：板级初始化与硬件版本标识。引脚定义见 board_pins.h。
 * 本层不依赖任何上层模块。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 板级初始化
 *
 * 应在 app_main() 中最先调用，早于任何外设驱动初始化。
 * 当前完成：打印板卡标识与芯片信息；后续新增共享总线（SPI/I2C）初始化。
 *
 * @return ESP_OK 成功
 */
esp_err_t board_init(void);
