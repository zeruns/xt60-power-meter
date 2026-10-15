/**
 * @file power_calc.c
 * @brief 功率测量纯计算逻辑实现（码值符号扩展 / 标定换算 / 功率 / 电能积分 / 过限判断）
 *
 * 本文件不依赖 ESP-IDF，也不依赖任何硬件，因此可以被 PC 编译器直接编译。
 * 设计初衷（详见 power_calc.h 的"测试金字塔"）：测量算法最容易在
 * 符号位、单位换算上出错，把这些纯函数拆出来，用主机单元测试
 * （test/host/test_power_calc.c）大量覆盖，比上板试错快两个数量级。
 *
 * 调用关系：power_measure.c 的 task_sample 每样本调用
 * power_calc_raw_to_value 换算 V/I，每 200ms 块调用 power_calc_energy_wh
 * 积分电能；本文件只做数学，不做任何 IO。
 */
#include "power_calc.h"

#include <stddef.h> /* 提供 NULL 定义；纯逻辑文件也不该隐式依赖别处的头文件 */

/**
 * @brief 把 24 位二进制补码原始值转换为有符号整数（符号扩展）
 *
 * ADS131M02 输出 24 位补码，而 C 的 int32_t 是 32 位——负数必须把
 * 高 8 位补成全 1（符号扩展），否则 -1 会变成 16777215 的巨大正数。
 * 详细踩坑分析见 power_calc.h 的 @brief。
 *
 * @param raw24 从 ADC 读回的 24 位数据（高位脏数据会被忽略）
 * @return 符号扩展后的有符号值，范围 -8388608 ~ 8388607
 */
int32_t power_calc_adc24_to_signed(uint32_t raw24)
{
    /* 只保留低 24 位，避免调用方误传高位脏数据造成结果不可预期 */
    raw24 &= 0x00FFFFFFu;

    /* 第 23 位是符号位。若为 1 说明是负数，
     * 需要把高 8 位全部补 1，才能得到正确的 32 位补码表示。 */
    if ((raw24 & 0x00800000u) != 0) {
        return (int32_t)(raw24 | 0xFF000000u);
    }

    return (int32_t)raw24;
}

/**
 * @brief 原始码值 → 物理量（线性标定）
 *
 * @param raw    有符号原始码值（应先用 power_calc_adc24_to_signed 转换）
 * @param calib  该通道的标定参数，不可为 NULL
 * @return 换算后的物理量（电压 V 或电流 A）
 */
float power_calc_raw_to_value(int32_t raw, const power_calc_channel_calib_t *calib)
{
    /* 防御性检查：指针为空时返回 0，避免段错误。
     * 纯逻辑函数保持"绝不崩溃"的特性，便于在主机上放心测试。 */
    if (calib == NULL) {
        return 0.0f;
    }

    /* 物理量 = 码值 × 每 LSB 对应量 + 零偏（scale 由调用方预算好，
     * 不在采样热路径里做除法，见 power_calc.h 的量纲说明） */
    return (float)raw * calib->scale + calib->offset;
}

/**
 * @brief 计算瞬时功率（电压 × 电流）
 *
 * 两通道是硬件同步采样的，直接相乘无相位误差（见 power_calc.h）。
 *
 * @param voltage_v 电压（V）
 * @param current_a 电流（A）
 * @return 瞬时功率（W）
 */
float power_calc_power_w(float voltage_v, float current_a)
{
    return voltage_v * current_a;
}

/**
 * @brief 矩形积分累加电能：energy += P × dt
 *
 * @param energy_wh  上一次的电能累计值（Wh）
 * @param power_w    本次的瞬时/平均功率（W）
 * @param dt_s       距上一次累加的时间间隔（秒）
 * @return 新的电能累计值（Wh）；dt 非法时原样返回
 */
float power_calc_energy_wh(float energy_wh, float power_w, float dt_s)
{
    /* 时间间隔非法时不累加，直接返回原值：
     * 这样可以避免除零、避免时间戳回退导致电能倒扣 */
    if (dt_s <= 0.0f) {
        return energy_wh;
    }

    /* 瓦·秒 → 瓦·时：除以 3600 */
    return energy_wh + power_w * dt_s / 3600.0f;
}

/**
 * @brief 判断是否超出安全范围（电压或电流任一超限即 true）
 *
 * @param voltage_v     当前电压（V）
 * @param current_a     当前电流（A）
 * @param max_voltage_v 允许的最高电压（V）
 * @param max_current_a 允许的最大电流（A）
 * @return true 表示超限，需要告警
 */
bool power_calc_is_over_limit(float voltage_v, float current_a, float max_voltage_v,
                              float max_current_a)
{
    /* 取绝对值比较：本工程允许双向电流（充/放电），
     * 因此 -X A 与 +X A 都应触发同样的过流保护。 */
    float abs_voltage = (voltage_v < 0.0f) ? -voltage_v : voltage_v;
    float abs_current = (current_a < 0.0f) ? -current_a : current_a;

    return (abs_voltage > max_voltage_v) || (abs_current > max_current_a);
}
