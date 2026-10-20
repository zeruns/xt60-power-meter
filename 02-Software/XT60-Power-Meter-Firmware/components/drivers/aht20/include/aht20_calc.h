/**
 * @file aht20_calc.h
 * @brief AHT20 与硬件/ESP-IDF 无关的纯计算逻辑（可在 PC 上做主机单元测试）
 *
 * 拆出这一层的原因与 power_calc / button_fsm 一致：
 * CRC8 与量程钳制不依赖任何硬件头文件，放到独立文件后既能被固件驱动直接调用，
 * 也能被 test/host 下的 gcc/clang 单元测试直接编译运行。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief AHT20 读数校验 CRC8
 *
 * 算法见 AHT20 说明书 §5.2：对 [状态][湿度20bit][温度20bit] 共 6 字节做 CRC8，
 * 初值 0xFF，多项式 x^8+x^5+x^4+1（0x31）；器件在第 7 字节返回该校验值。
 *
 * @param data 状态+数据的 6 字节缓冲
 * @param len  字节数（AHT20 固定为 6）
 * @return 计算得到的 CRC8（应与器件返回的第 7 字节相等）
 */
uint8_t aht20_crc8(const uint8_t *data, size_t len);

/**
 * @brief 把相对湿度钳制到物理量程 [0,100] %RH
 *
 * 相对湿度物理上不可能小于 0 或大于 100；超出通常意味着通信/数据异常。
 * 数据链路另有 CRC 校验拦截传输错误，这里只做最终的物理量防护。
 *
 * @param humidity_pct 原始换算得到的相对湿度
 * @return 钳制后的相对湿度
 */
float aht20_clamp_rh(float humidity_pct);
