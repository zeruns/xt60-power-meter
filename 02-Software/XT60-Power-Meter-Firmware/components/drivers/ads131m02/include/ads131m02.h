/**
 * @file ads131m02.h
 * @brief ADS131M02 双通道 24bit 同步采样 ΔΣ ADC 驱动（SPI）
 *
 * 通道分配：
 *   AIN0 —— 电压，PGA 增益 1x，前端 101:1 精密分压（R33 100kΩ / R39 1kΩ）
 *   AIN1 —— 电流，PGA 增益 16x，前端 1mΩ 四端开尔文分流器
 * 引脚见 board_pins.h 的 BOARD_PIN_ADC_* 与 BOARD_PIN_ADC_CLK。
 *
 * 实现要点见 src/ads131m02.c 文件头：SPI 时序（mode 1）、本板无片选(CS)的处理方式、
 * 时钟来源 R27/R29 的说明，以及完整的「上板调试要点」。
 */
#pragma once

#include "esp_err.h"

/** 一次同步采样结果（ADC 原始码值） */
typedef struct {
    int32_t voltage_raw; /**< AIN0 原始码值（24bit 符号扩展） */
    int32_t current_raw; /**< AIN1 原始码值（24bit 符号扩展） */
    uint32_t status;     /**< 本帧 STATUS 字（高 16bit 有效，见手册 §8.6.2） */
} ads131m02_sample_t;

/**
 * @brief 初始化 ADS131M02（配置 SPI、复位时序、寄存器与 PGA 增益）
 * @return ESP_OK 成功
 */
esp_err_t ads131m02_init(void);

/**
 * @brief 读取一组同步采样数据
 * @param out 输出：两通道原始码值，不可为 NULL
 * @return ESP_OK 成功；ESP_ERR_TIMEOUT 等待 DRDY# 超时
 */
esp_err_t ads131m02_read(ads131m02_sample_t *out);

/**
 * @brief 查询配置的输出数据率（ODR）
 *
 * init() 会把 CLOCK 寄存器显式配置为 HR 模式 + OSR=1024：
 *   f_MOD = f_CLKIN / 2 = 8.192MHz / 2 = 4.096MHz
 *   ODR   = f_MOD / OSR = 4.096MHz / 1024 = 4000 Hz（本工程默认采样率）
 *
 * @return 采样率（Hz）；驱动未初始化时返回 0
 */
uint32_t ads131m02_get_sample_rate_hz(void);
