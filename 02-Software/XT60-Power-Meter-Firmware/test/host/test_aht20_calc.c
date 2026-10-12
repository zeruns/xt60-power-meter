/**
 * @file test_aht20_calc.c
 * @brief AHT20 纯逻辑（CRC8 校验、湿度量程钳制）单元测试（PC 运行，无需开发板）
 *
 * 覆盖点（均为“总线干扰下最容易出错、上板最难发现”的逻辑）：
 *   · CRC8（初值 0xFF、多项式 0x31）与一个独立的查表参考实现一致；
 *   · 任意 1 bit 翻转都应导致 CRC 不匹配（保证校验真正有检错能力）；
 *   · 相对湿度物理量程 [0,100]%RH 的钳制边界。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aht20_calc.h"

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(cond)                                                                                     \
    do {                                                                                                \
        if (cond) {                                                                                     \
            s_pass++;                                                                                   \
        } else {                                                                                        \
            s_fail++;                                                                                   \
            printf("[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);                                    \
        }                                                                                               \
    } while (0)

/* ---- 独立的 CRC8 参考实现（运行时建表，与驱动里的逐位实现互不共用代码） ---- */
static uint8_t ref_crc8_table[256];

static void ref_crc8_build_table(void)
{
    for (uint16_t i = 0; i < 256; i++) {
        uint8_t crc = (uint8_t)i;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x31u) : (uint8_t)(crc << 1);
        }
        ref_crc8_table[i] = crc;
    }
}

static uint8_t ref_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc = (uint8_t)(ref_crc8_table[crc ^ data[i]]);
    }
    return crc;
}

/* 线性同余生成确定性伪随机帧（无需真实传感器即可覆盖大量组合） */
static void make_frame(uint8_t frame[6], uint32_t seed)
{
    uint32_t s = seed ? seed : 1u;
    for (int i = 0; i < 6; i++) {
        s = s * 1103515245u + 12345u;
        frame[i] = (uint8_t)(s >> 16);
    }
}

int main(void)
{
    ref_crc8_build_table();

    /* 1) 驱动实现与独立参考实现在大量随机帧上结果一致（含状态字节） */
    for (uint32_t seed = 1; seed <= 2000; seed++) {
        uint8_t frame[6];
        make_frame(frame, seed);
        CHECK(aht20_crc8(frame, 6) == ref_crc8(frame, 6));
    }

    /* 2) 全零帧的确定性向量（两实现一致） */
    {
        uint8_t zeros[6] = { 0 };
        CHECK(aht20_crc8(zeros, 6) == ref_crc8(zeros, 6));
    }

    /* 3) 单 bit 翻转必被检出：对若干帧逐位翻转，CRC 必须变化 */
    for (uint32_t seed = 1; seed <= 30; seed++) {
        uint8_t frame[6];
        make_frame(frame, seed);
        uint8_t good = aht20_crc8(frame, 6);
        for (int byte = 0; byte < 6; byte++) {
            for (int bit = 0; bit < 8; bit++) {
                uint8_t tampered[6];
                memcpy(tampered, frame, sizeof(tampered));
                tampered[byte] ^= (uint8_t)(1u << bit);
                CHECK(aht20_crc8(tampered, 6) != good);
            }
        }
    }

    /* 4) 湿度量程钳制边界 */
    CHECK(aht20_clamp_rh(-5.0f) == 0.0f);
    CHECK(aht20_clamp_rh(105.0f) == 100.0f);
    CHECK(aht20_clamp_rh(0.0f) == 0.0f);
    CHECK(aht20_clamp_rh(100.0f) == 100.0f);
    CHECK(aht20_clamp_rh(42.5f) == 42.5f);

    printf("\naht20_calc: %d 通过, %d 失败\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
