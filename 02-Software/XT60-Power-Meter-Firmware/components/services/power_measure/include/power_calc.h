/**
 * @file power_calc.h
 * @brief 功率测量的纯计算逻辑（不依赖任何硬件与操作系统）
 *
 * ─────────────────────────────────────────────────────────────
 * 为什么要把这部分单独拆出来？
 * ─────────────────────────────────────────────────────────────
 * 嵌入式开发最大的痛点是"必须上板才能验证"：改一行代码要编译、
 * 烧录、接线、测波形，一轮几分钟。而测量算法这类逻辑恰恰最容易
 * 出错（尤其符号位、单位换算），却最不该靠上板试错。
 *
 * 本文件刻意做到：
 *   · 只使用标准 C 头文件（stdint/stdbool），不包含任何 ESP-IDF 头；
 *   · 不访问硬件、不使用全局状态、不使用动态内存；
 *   · 每个函数都是"给输入就有确定的输出"的纯函数。
 *
 * 于是它可以被 PC 上的编译器直接编译并用单元测试覆盖
 * （见 test/host/test_power_calc.c），跑上百个用例只要一秒，
 * 比上板验证快两个数量级。
 *
 * 这也是"测试金字塔"在嵌入式里的落地方式：
 *   底层——纯算法，主机上大量快速测试；
 *   中层——驱动，用模拟/桩件测寄存器解析逻辑；
 *   顶层——整机联调，数量少但最真实。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 单个测量通道的标定参数
 *
 * 换算公式：物理量 = 原始码值 × scale + offset
 *
 * 其中 scale 需要把多级因素合并进来：
 *   电压通道：ADC 满量程电压 / 2^23 ÷ PGA增益 × 分压比(本板为 101)
 *   电流通道：ADC 满量程电压 / 2^23 ÷ PGA增益 ÷ 分流电阻(本板 1mΩ)
 * 因此调用方应在初始化时把标定结果算好传进来，
 * 避免在 500Hz 的采样循环里做除法。
 */
typedef struct {
    float scale;  /**< 每个原始码值对应的物理量（V/LSB 或 A/LSB） */
    float offset; /**< 零点偏置（用于消除失调） */
} power_calc_channel_calib_t;

/**
 * @brief 把 24 位二进制补码原始值转换为有符号整数
 *
 * ── 这是本文件最重要的一个函数，也是最容易写错的地方 ──
 * ADS131M02 输出 24 位二进制补码。若直接把读回的 3 字节拼成 uint32 使用，
 * 负值会变成接近 16777215 的巨大正数（例如实际 -1 会变成 16777215），
 * 表现为"电流方向反了"或"读数突然跳到满量程"，且只在负半周出现，极难排查。
 *
 * 正确做法：判断第 23 位（最高有效位）是否为 1，是则把高 8 位补成全 1，
 * 完成"符号扩展"到 32 位。
 *
 * @param raw24 从 ADC 读回的 24 位数据（放在入参的低 24 位，高位会被忽略）
 * @return 正确符号扩展后的有符号值，范围 -8388608 ~ 8388607
 */
int32_t power_calc_adc24_to_signed(uint32_t raw24);

/**
 * @brief 原始码值 → 物理量
 *
 * @param raw    有符号原始码值（应先用 power_calc_adc24_to_signed 转换）
 * @param calib  该通道的标定参数，不可为 NULL
 * @return 换算后的物理量（电压 V 或电流 A）
 */
float power_calc_raw_to_value(int32_t raw, const power_calc_channel_calib_t *calib);

/**
 * @brief 计算瞬时功率
 *
 * 电压与电流是两个通道「硬件同步采样」得到的，因此可以直接相乘，
 * 不需要考虑相位补偿——这是选用 ADS131M02 这类同步采样 ADC 的意义所在。
 *
 * @param voltage_v 电压（V）
 * @param current_a 电流（A）
 * @return 瞬时功率（W）
 */
float power_calc_power_w(float voltage_v, float current_a);

/**
 * @brief 累加电能
 *
 * 电能 = ∑(功率 × 时间)，单位换算：1 小时 = 3600 秒，
 * 所以把"瓦·秒"换算成"瓦·时"需要除以 3600。
 *
 * 说明：这里用矩形积分（取当前功率乘以时间间隔）。
 *       对缓慢变化的负载足够准确；若负载变化剧烈，
 *       应改为梯形积分（用前后两次功率的平均值）。
 *
 * @param energy_wh  上一次的电能累计值（Wh）
 * @param power_w    本次采样的瞬时功率（W）
 * @param dt_s       距上一次累加的时间间隔（秒），必须 > 0
 * @return 新的电能累计值（Wh）
 */
float power_calc_energy_wh(float energy_wh, float power_w, float dt_s);

/**
 * @brief 判断是否超出安全范围
 *
 * @param voltage_v   当前电压（V）
 * @param current_a   当前电流（A）
 * @param max_voltage_v 允许的最高电压（V）
 * @param max_current_a 允许的最大电流（A）
 * @return true 表示超限，需要告警
 */
bool power_calc_is_over_limit(float voltage_v, float current_a, float max_voltage_v,
                              float max_current_a);
