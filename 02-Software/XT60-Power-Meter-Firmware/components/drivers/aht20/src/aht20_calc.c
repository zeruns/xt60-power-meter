/**
 * @file aht20_calc.c
 * @brief AHT20 纯计算逻辑实现：CRC8 校验与湿度量程钳制
 */
#include "aht20_calc.h"

uint8_t aht20_crc8(const uint8_t *data, size_t len)
{
    /* AHT20 说明书给出的参考实现：初值 0xFF、多项式 0x31（x8+x5+x4+1） */
    uint8_t crc = 0xFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x31u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

float aht20_clamp_rh(float humidity_pct)
{
    if (humidity_pct < 0.0f) {
        return 0.0f;
    }
    if (humidity_pct > 100.0f) {
        return 100.0f;
    }
    return humidity_pct;
}
