/**
 * @file ads131m02.h
 * @brief ADS131M02 双通道 24bit 同步采样 ΔΣ ADC 驱动（SPI）
 *
 * 通道分配（⚠ 与直觉相反，依据上板原理图 V2 的实际接法）：
 *   AIN0（CH0）—— **电流**，PGA 增益 16x，前端 1mΩ 四端开尔文分流器（ISENSE±）
 *   AIN1（CH1）—— **电压**，PGA 增益 1x，前端 U7 电压跟随器 + 101:1 精密分压
 *                 （R33 100kΩ / R39 1kΩ）
 * 原理图标注印证：电压量程 121V@1x、电流量程 ±75A@16x。
 * 引脚见 board_pins.h 的 BOARD_PIN_ADC_* 与 BOARD_PIN_ADC_CLK。
 *
 * 实现要点见 src/ads131m02.c 文件头：SPI 时序（mode 1）、本板无片选(CS)的处理方式、
 * 时钟来源 R27/R29 的说明，以及完整的「上板调试要点」。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** 一次同步采样结果（ADC 原始码值） */
typedef struct {
    int32_t voltage_raw; /**< 电压通道 CH1/AIN1 原始码值（24bit 符号扩展，PGA 1x） */
    int32_t current_raw; /**< 电流通道 CH0/AIN0 原始码值（24bit 符号扩展，PGA 16x） */
    uint32_t status;     /**< 本帧 STATUS 字（高 16bit 有效，见手册 §8.6.2） */
    bool crc_ok;         /**< 帧输出 CRC-16 校验结果（覆盖 STATUS+CH0+CH1）；
                          *  false = 帧错位/损坏，数据不可用（毛刺剔除判据） */
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
 * @brief 芯片存在性与数据可信度自检（启动自检用）
 *
 * 比 ads131m02_read() 更严格——read() 只验证「DRDY 来了 + SPI 没报错」，
 * 芯片没焊（MISO 悬空读 0）时也可能通过，表现为"自检通过但测量全 0"。
 * 本函数补三道闸门：
 *   ① ID 寄存器高字节必须是 0x22（芯片存在性通信锚点，见实现注释）；
 *   ② GAIN1 写读一致（寄存器通路可信）；
 *   ③ 连续多帧电压/电流码值不能全 0（本设备由被测电源供电，全 0 说明
 *      芯片缺失或测量前端未接入）。
 *
 * @return ESP_OK 自检通过；其他为对应失败原因（日志会写明诊断结论）
 */
esp_err_t ads131m02_selftest(void);

/**
 * @brief DRDY# 中断累计边沿数（诊断用：区分「DRDY 真的慢」还是「任务处理慢」）
 */
uint32_t ads131m02_drdy_count(void);

/** 驱动运行统计（M1 基线验收与长期健康监控；自开机累计，不复位） */
typedef struct {
    uint32_t drdy_count;   /**< DRDY# 下降沿累计 */
    uint32_t edge_ok;      /**< 引擎成功 kick 的帧数 */
    uint32_t edge_busy;    /**< 槽满丢弃的流式帧数（消费者落后时的丢帧） */
    uint32_t read_ok;      /**< 成功读取帧数 */
    uint32_t crc_fail;     /**< CRC 校验失败帧数（已剔除，不可用数据） */
    uint32_t read_timeout; /**< 等 DRDY 超时次数 */
    uint32_t lat_min_us;   /**< DRDY 边沿 → SPI 事务启动延迟 δ（µs）最小值 */
    uint32_t lat_avg_us;   /**< δ（µs）均值 */
    uint32_t lat_max_us;   /**< δ（µs）最大值 */
} ads131m02_stats_t;

/**
 * @brief 获取驱动运行统计（诊断/验收用）
 *
 * M1 基线验收判据（docs/ads131-4khz-implementation-plan.md §1）：
 *   DRDY ≈ 4000/s 持续成立；δ p99≤20µs（看 min/avg/max 分布）；
 *   crc_fail = 0、read_timeout = 0。
 *
 * @param out 输出统计快照，不可为 NULL
 */
void ads131m02_get_stats(ads131m02_stats_t *out);

/**
 * @brief 查询配置的输出数据率（ODR）
 *
 * init() 会把 CLOCK 寄存器显式配置为 HR 模式 + **OSR=1024 档**
 * （ADS_CLOCK_VALUE=0x030E，恰为芯片复位值）：
 *   f_MOD = f_CLKIN / 2 = 8.192MHz / 2 = 4.096MHz
 *   ODR   = f_MOD / OSR = 4.096MHz / 1024 = 4000 Hz（本工程采样率）
 *
 * ⚠ 4kHz 曾不达标的原因与恢复依据：手册 §8.5.1.5 明确 DRDY 会在
 * 「读取窗口跨越转换完成时刻」时被抑制。旧版 4MHz SPI + 任务调度路径下
 * "等 DRDY+读帧"周期约 3ms，在 250µs 转换周期内必然跨越，DRDY 实际塌到
 * ~310Hz（当时以 OSR=8192/ODR=500Hz 加余量绕过）。现以 20MHz SPI + DMA
 * 把读窗压到 µs 级，读窗固定在 DRDY 之后启动、远离转换完成时刻，机制上
 * 避开抑制条件；DRDY→SPI 启动延迟 δ 由 ads131m02_get_stats() 实测监控。
 * （详见 src/ads131m02.c 的 ADS_CLOCK_* / ADS_SPI_CLK_HZ 注释。）
 *
 * @return 采样率（Hz）；驱动未初始化时返回 0
 */
uint32_t ads131m02_get_sample_rate_hz(void);
