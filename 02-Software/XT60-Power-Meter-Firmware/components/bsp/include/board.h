/**
 * @file board.h
 * @brief 板级支持层对外接口
 *
 * 职责：板级初始化与硬件版本标识。引脚定义见 board_pins.h。
 * 本层不依赖任何上层模块。
 */
#pragma once

#include "driver/i2c_master.h"
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

/**
 * @brief 获取「传感器 I2C 总线」句柄（create-or-get）
 *
 * AHT20（0x38）与 SC7A20H（0x19）挂载在同一组 I2C1（SCL=IO21 / SDA=IO47）上。
 * 两个器件必须共用同一个 i2c_master_bus 句柄，不能各自 i2c_new_master_bus()
 * —— 那会为同一个 I2C 外设重复创建总线控制器，导致冲突。
 *
 * 因此由 bsp 层统一持有该总线：首次调用时创建，之后返回同一句柄。
 * 各传感器驱动拿到总线后，再各自 i2c_master_bus_add_device() 挂载自己的器件。
 *
 * 启动期为顺序单线程执行，create-or-get 无需加锁；运行期多任务并发读写
 * 由 i2c_master 驱动内部保证总线级互斥，但建议上层对单个器件的读写自行串行化。
 *
 * @param out 输出：总线句柄，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法；其他值表示总线创建失败
 */
esp_err_t board_i2c_sensor_bus_get(i2c_master_bus_handle_t *out);
