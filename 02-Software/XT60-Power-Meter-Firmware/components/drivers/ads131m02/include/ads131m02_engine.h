/**
 * @file ads131m02_engine.h
 * @brief ADS131M02 数据面引擎（M2）：DRDY ISR 直启 GDMA 的最小 SPI3 帧引擎
 *
 * 为什么自研引擎而不用 spi_master（设计依据见
 * docs/ads131-4khz-implementation-plan.md §4.2）：
 *   spi_master 的事务启动 API 全部是「任务上下文」语义——queue_trans 入队即
 *   启动、polling_start 不可在 ISR 调用（内部取互斥锁），无法做「DRDY 边沿后
 *   立即、确定性地」发起读帧。本引擎把「启动一帧」压缩成 ISR 内的几条
 *   寄存器写（DMA 描述符重臂 + spi_ll_user_start），读窗相位抖动从任务调度
 *   级（几十 µs）降到中断级（~1 µs）。
 *
 * 帧格式固定不变：96 bit = 4×24bit 字（STATUS/CH0/CH1/CRC），SPI mode 1，
 * 全双工。TX 侧每帧发 NULL（0x00）或维护命令字，RX 侧收整帧。
 *
 * IRAM 安全约定（NVS 擦写关 cache 窗口不崩溃，见 sdkconfig.defaults）：
 *   DRDY ISR / kick_frame 只做：内存写（描述符）+ inline LL 调用
 *   （spi_ll_* / gdma_ll_*），全部 IRAM 安全；任务上下文才用驱动/HAL API。
 *
 * 并发模型：
 *   · kick 只有两个发起者——DRDY ISR（流式）与维护事务（任务上下文，此时
 *     流式已暂停），互斥由 s_stream_on 原子开关保证，绝不并发 kick；
 *   · 槽环（s_slot）由 kick 写、消费读，kicked/read 序号单调，容量 8，
 *     ISR kick 前检查余量，满则丢帧计数（绝不覆盖未消费槽）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 一帧 = 4 个 24bit 字（STATUS+CH0+CH1+CRC），引擎的传输粒度 */
#define ADS_ENGINE_FRAME_WORDS 4

/** 引擎统计（诊断/验收用；自开机累计） */
typedef struct {
    uint32_t drdy_count;    /**< DRDY# 边沿计数 */
    uint32_t kick_count;    /**< 发起的帧数（含维护帧） */
    uint32_t drop_count;    /**< 槽满丢弃的流式帧数 */
    uint32_t lat_min_us;    /**< DRDY 边沿 → SPI 启动延迟 δ（µs）最小值 */
    uint32_t lat_avg_us;    /**< δ（µs）均值 */
    uint32_t lat_max_us;    /**< δ（µs）最大值 */
} ads_engine_stats_t;

/**
 * @brief 初始化 SPI3 + GDMA 引擎（任务上下文调用）
 *
 * 完成：引脚矩阵接线、SPI3 外设使能与时钟配置（HAL）、GDMA 双通道接线、
 * 固定 96bit 帧格式装载。不做寄存器访问——那由上层用 xfer_frame 完成。
 *
 * @param mosi/miso/sclk 三个 SPI 引脚 GPIO 号
 * @param clk_hz         目标 SCLK（Hz，如 20MHz；引擎不裁剪，调用方护栏）
 * @param input_delay_ns 从机数据有效延迟（t_p(SCDO)，时序补偿用）
 * @return ESP_OK 成功
 */
esp_err_t ads_engine_init(int mosi, int miso, int sclk, int clk_hz, int input_delay_ns);

/**
 * @brief 安装 DRDY# 下降沿中断（ISR 内直接 kick 流式帧）
 *
 * @param drdy_gpio DRDY# 引脚 GPIO 号（低有效，内部上拉）
 * @return ESP_OK 成功
 */
esp_err_t ads_engine_attach_drdy(int drdy_gpio);

/**
 * @brief 开/关流式 kick（维护模式前关、后开；关闭时 DRDY ISR 只打时间戳）
 * @param on true=开流式
 * @return 之前的开关状态（便于"临时暂停-恢复"：保存旧值，用后恢复）
 */
bool ads_engine_set_stream(bool on);

/**
 * @brief DRDY 同步的单帧交换（维护/寄存器访问用；流式应已暂停）
 *
 * 「等 DRDY → kick 一帧 → 等完成」的不可分割序列，帧边界仍由 DRDY 界定
 * （本板无 CS）。tx/rx 为 4 个 24bit 字（tx 可为 NULL 表示全 0）。
 *
 * @param tx  待发送 4 字（NULL=全 NULL 帧）
 * @param rx  输出 4 字
 * @param timeout_ms 等 DRDY 超时（ms）
 * @return ESP_OK / ESP_ERR_TIMEOUT
 */
esp_err_t ads_engine_xfer_frame(const uint32_t *tx, uint32_t *rx, uint32_t timeout_ms);

/**
 * @brief 流式读一帧（消费 DRDY ISR kick 的帧）
 *
 * 等待「帧已 kick」信号 → 等 DMA 完成 → 从槽环取帧。丢帧时序不齐会返回
 * ESP_ERR_TIMEOUT（调用方按丢帧处理）。
 *
 * @param rx 输出 4 个 24bit 字
 * @param timeout_ms 超时（ms）
 * @return ESP_OK / ESP_ERR_TIMEOUT
 */
esp_err_t ads_engine_read_frame(uint32_t *rx, uint32_t timeout_ms);

/**
 * @brief 获取引擎统计
 * @param out 输出快照，不可为 NULL
 */
void ads_engine_get_stats(ads_engine_stats_t *out);
