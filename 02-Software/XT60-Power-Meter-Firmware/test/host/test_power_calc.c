/**
 * @file test_power_calc.c
 * @brief power_calc 纯逻辑的单元测试（在 PC 上运行，不需要开发板）
 *
 * ─────────────────────────────────────────────────────────────
 * 怎么运行？
 * ─────────────────────────────────────────────────────────────
 * 本机若装有 gcc / clang，直接编译运行即可：
 *
 *   gcc -std=c11 -Wall -Wextra -I../../components/services/power_measure/include \
 *       test_power_calc.c \
 *       ../../components/services/power_measure/src/power_calc.c \
 *       -o test_power_calc.exe
 *   ./test_power_calc.exe
 *
 * 或使用同目录下的 run_host_tests.ps1 脚本（会自动寻找可用编译器）。
 *
 * ─────────────────────────────────────────────────────────────
 * 为什么值得写这个文件？
 * ─────────────────────────────────────────────────────────────
 * 下面这些用例覆盖的都是"最容易写错、但上板又最难发现"的点：
 *   · 24 位补码的符号扩展（错了会导致负电流被读成满量程正值）
 *   · 电能单位换算（瓦秒 → 瓦时，漏除 3600 会差 3600 倍）
 *   · 边界值与负数（过流保护只判断正向就会漏保护）
 * 在 PC 上跑这些用例只需要毫秒级时间，却能把它们从"靠上板试"变成"必然正确"。
 */
#include <math.h>
#include <stdio.h>

#include "power_calc.h"

/** 测试计数器 */
static int s_pass = 0;
static int s_fail = 0;

/** 浮点比较的容许误差：单精度浮点运算必然有微小误差，不能直接用 == 比较 */
#define FLOAT_EPSILON 1e-4f

/**
 * @brief 断言一个条件成立
 */
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (cond) {                                                                                \
            s_pass++;                                                                              \
        } else {                                                                                   \
            s_fail++;                                                                              \
            printf("  [失败] 第 %d 行: %s\n", __LINE__, #cond);                                    \
        }                                                                                          \
    } while (0)

/**
 * @brief 断言两个浮点数近似相等
 */
#define CHECK_NEAR(actual, expected)                                                               \
    do {                                                                                           \
        float a_ = (actual);                                                                       \
        float e_ = (expected);                                                                     \
        if (fabsf(a_ - e_) <= FLOAT_EPSILON) {                                                     \
            s_pass++;                                                                              \
        } else {                                                                                   \
            s_fail++;                                                                              \
            printf("  [失败] 第 %d 行: 实际 %f, 期望 %f\n", __LINE__, a_, e_);                     \
        }                                                                                          \
    } while (0)

/**
 * @brief 测试 24 位补码 → 有符号整数（本文件最重要的测试组）
 */
static void test_adc24_to_signed(void)
{
    printf("测试 power_calc_adc24_to_signed:\n");

    /* 零值 */
    CHECK(power_calc_adc24_to_signed(0x000000u) == 0);

    /* 正数边界：24 位有符号最大值为 0x7FFFFF = 8388607 */
    CHECK(power_calc_adc24_to_signed(0x7FFFFFu) == 8388607);
    CHECK(power_calc_adc24_to_signed(0x000001u) == 1);

    /* 负数边界：24 位有符号最小值为 0x800000 = -8388608 */
    CHECK(power_calc_adc24_to_signed(0x800000u) == -8388608);

    /* -1 在 24 位补码里是全 1。这是最容易出错的一例：
     * 若不做符号扩展，会被当成 16777215 使用。 */
    CHECK(power_calc_adc24_to_signed(0xFFFFFFu) == -1);
    CHECK(power_calc_adc24_to_signed(0xFFFFFEu) == -2);
    CHECK(power_calc_adc24_to_signed(0xFF0000u) == -65536);

    /* 高位脏数据必须被忽略，只取低 24 位 */
    CHECK(power_calc_adc24_to_signed(0xABFFFFFFu) == -1);
    CHECK(power_calc_adc24_to_signed(0x12000001u) == 1);
}

/**
 * @brief 测试原始码值 → 物理量的换算
 */
static void test_raw_to_value(void)
{
    printf("测试 power_calc_raw_to_value:\n");

    /* 电压通道：假设标定后每 LSB = 0.01V，无偏置 */
    const power_calc_channel_calib_t voltage_calib = { .scale = 0.01f, .offset = 0.0f };
    CHECK_NEAR(power_calc_raw_to_value(1000, &voltage_calib), 10.0f);
    CHECK_NEAR(power_calc_raw_to_value(-1000, &voltage_calib), -10.0f);

    /* 带偏置：用于消除运放失调 */
    const power_calc_channel_calib_t current_calib = { .scale = 0.001f, .offset = 0.5f };
    CHECK_NEAR(power_calc_raw_to_value(0, &current_calib), 0.5f);
    CHECK_NEAR(power_calc_raw_to_value(2000, &current_calib), 2.5f);

    /* 防御性：空指针应返回 0 而不是崩溃 */
    CHECK_NEAR(power_calc_raw_to_value(123, NULL), 0.0f);
}

/**
 * @brief 测试功率计算
 */
static void test_power(void)
{
    printf("测试 power_calc_power_w:\n");

    CHECK_NEAR(power_calc_power_w(10.0f, 2.0f), 20.0f);
    CHECK_NEAR(power_calc_power_w(0.0f, 5.0f), 0.0f);

    /* 反向电流（放电）时功率为负，这是正确行为，不应被"修正" */
    CHECK_NEAR(power_calc_power_w(12.0f, -1.0f), -12.0f);
}

/**
 * @brief 测试电能累加
 */
static void test_energy(void)
{
    printf("测试 power_calc_energy_wh:\n");

    /* 100W 持续 36 秒 = 3600 瓦秒 = 1 瓦时 */
    CHECK_NEAR(power_calc_energy_wh(0.0f, 100.0f, 36.0f), 1.0f);

    /* 累加性质：再持续 36 秒应变成 2 瓦时 */
    float e = power_calc_energy_wh(0.0f, 100.0f, 36.0f);
    CHECK_NEAR(power_calc_energy_wh(e, 100.0f, 36.0f), 2.0f);

    /* 非法时间间隔不应改变累计值（防止除零与时间戳回退把电量扣掉） */
    CHECK_NEAR(power_calc_energy_wh(5.0f, 100.0f, 0.0f), 5.0f);
    CHECK_NEAR(power_calc_energy_wh(5.0f, 100.0f, -1.0f), 5.0f);

    /* 零功率不产生电能 */
    CHECK_NEAR(power_calc_energy_wh(3.0f, 0.0f, 10.0f), 3.0f);
}

/**
 * @brief 测试超限判断
 */
static void test_over_limit(void)
{
    printf("测试 power_calc_is_over_limit:\n");

    const float max_v = 100.0f;
    const float max_i = 60.0f;

    /* 正常范围 */
    CHECK(!power_calc_is_over_limit(50.0f, 30.0f, max_v, max_i));

    /* 恰好等于上限不算超限（边界语义需明确，避免保护点抖动） */
    CHECK(!power_calc_is_over_limit(100.0f, 60.0f, max_v, max_i));

    /* 正向超限 */
    CHECK(power_calc_is_over_limit(101.0f, 10.0f, max_v, max_i));
    CHECK(power_calc_is_over_limit(50.0f, 61.0f, max_v, max_i));

    /* 反向超限：双向电流（充放电）都要保护，
     * 这是"只判断正向"实现会漏掉的情况 */
    CHECK(power_calc_is_over_limit(-101.0f, 10.0f, max_v, max_i));
    CHECK(power_calc_is_over_limit(50.0f, -61.0f, max_v, max_i));
}

int main(void)
{
    printf("===== power_calc 单元测试 =====\n");

    test_adc24_to_signed();
    test_raw_to_value();
    test_power();
    test_energy();
    test_over_limit();

    printf("===== 结果: 通过 %d, 失败 %d =====\n", s_pass, s_fail);

    /* 返回非 0 表示测试失败，便于 CI / 脚本判断 */
    return (s_fail == 0) ? 0 : 1;
}
