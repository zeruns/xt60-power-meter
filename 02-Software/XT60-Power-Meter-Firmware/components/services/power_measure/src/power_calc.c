/**
 * @file power_calc.c
 * @brief 功率测量纯计算逻辑实现
 *
 * 本文件不依赖 ESP-IDF，也不依赖任何硬件，因此可以被 PC 编译器直接编译。
 */
#include "power_calc.h"

#include <stddef.h> /* 提供 NULL 定义；纯逻辑文件也不该隐式依赖别处的头文件 */

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

float power_calc_raw_to_value(int32_t raw, const power_calc_channel_calib_t *calib)
{
    /* 防御性检查：指针为空时返回 0，避免段错误。
     * 纯逻辑函数保持"绝不崩溃"的特性，便于在主机上放心测试。 */
    if (calib == NULL) {
        return 0.0f;
    }

    return (float)raw * calib->scale + calib->offset;
}

float power_calc_power_w(float voltage_v, float current_a)
{
    return voltage_v * current_a;
}

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

bool power_calc_is_over_limit(float voltage_v, float current_a, float max_voltage_v,
                              float max_current_a)
{
    /* 取绝对值比较：本工程允许双向电流（充/放电），
     * 因此 -X A 与 +X A 都应触发同样的过流保护。 */
    float abs_voltage = (voltage_v < 0.0f) ? -voltage_v : voltage_v;
    float abs_current = (current_a < 0.0f) ? -current_a : current_a;

    return (abs_voltage > max_voltage_v) || (abs_current > max_current_a);
}
