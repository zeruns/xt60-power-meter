/**
 * @file imu_sc7a20h.c
 * @brief SC7A20HTR 三轴加速度计驱动实现（I2C，7 位地址 0x19）
 *
 * 器件：SC7A20HTR 三轴加速度计，I2C 7 位地址 0x19（BOARD_SC7A20_I2C_ADDR）。
 * 引脚：SCL=IO21、SDA=IO47（与 AHT20 共享「传感器 I2C 总线」，见 board.h）。
 * 用途：读取重力方向，判定握持朝向，供屏幕自动旋转。
 *
 * 阅读顺序建议：
 *   ① 寄存器映射（下方 REG_*，兼容 ST LIS2DH12/LIS3DH）；
 *   ② imu_sc7a20h_init()             —— 挂载器件 + 配 ODR/量程 + 自检；
 *   ③ imu_sc7a20h_get_orientation()  —— 读三轴 -> 重力主导轴法判朝向。
 *
 * ============================================================
 * 为什么自实现
 * ============================================================
 * SC7A20H 无乐鑫官方托管组件（仅第三方），且需与 AHT20 共享同一 I2C 外设，
 * 由 bsp 的 board_i2c_sensor_bus_get() 以官方 esp_driver_i2c 新版 i2c_master
 * 统一持有总线。故基于官方底层驱动自实现，寄存器兼容 ST LIS2DH12/LIS3DH。
 *
 * ============================================================
 * 寄存器要点（LIS2DH12 兼容）
 * ============================================================
 *   WHO_AM_I (0x0F)   ：器件 ID，LIS2DH12=0x11；SC7A20H 多为 0x11 或 0x13。
 *   CTRL_REG1 (0x20)  ：ODR[7:4] | LPen[3] | Z/Y/Xen[2:0]。ODR=0b0101(100Hz)，
 *                       三轴使能=0b111 -> 0x57。
 *   CTRL_REG4 (0x23)  ：bit7=BDU(块更新) | bit[6]=BLE(字节序) | bit[5:4]=FS | bit3=DLPF0
 *                       | bit[2:1]=ST | bit0=SIM。BDU=1、FS=00(±2g) -> 0x80。
 *                       注意与 LIS2DH12 的差异：本芯片的 HR（高性能/12bit）位不在
 *                       0x23，而在 CTRL_REG0(0x1F) 的 bit0（复位默认 0=正常模式）。
 *                       本驱动仅用主导轴/符号判朝向（与分辨率无关），无需使能 HR。
 *   OUT_X_L (0x28)~OUT_Z_H (0x2D)：6 字节三轴数据，低字节在前，16bit 补码。
 *   连续读：寄存器地址最高位置 1（0x80|0x28=0xA8）触发地址自增。
 *
 * ============================================================
 * 上板调试要点（方向判定不准时排查）
 * ============================================================
 *   · init 报 ID 不符：核对 I2C 地址 0x19；SC7A20H 批次 ID 可能非 0x11，
 *     代码对 ID 仅告警不致命，能读到三轴即认为可用。
 *   · 朝向映射反了 / 与实际安装不符：下面 get_orientation 的「符号 -> 朝向」
 *     映射依赖屏幕与芯片的相对安装方向，需按实际装配微调（对照调试）。
 *   · 平放（重力沿 Z）时不切换：设计如此——平放无法区分横竖，保持上次朝向。
 * ============================================================
 */
#include "imu_sc7a20h.h"

#include <stdlib.h>

#include "board.h"
#include "board_pins.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "imu_sc7a20h";

/* I2C / 器件参数 */
#define IMU_I2C_ADDR         BOARD_SC7A20_I2C_ADDR /**< 0x19 */
#define IMU_I2C_FREQ_HZ      400000                /**< 400kHz 快速模式 */
#define IMU_I2C_TIMEOUT_MS   50

/** 器件 ID（LIS2DH12 兼容），仅用于自检告警 */
#define IMU_WHO_AM_I_EXPECT  0x11

/* 寄存器地址（LIS2DH12 兼容） */
#define IMU_REG_WHO_AM_I   0x0F
#define IMU_REG_CTRL1      0x20
#define IMU_REG_CTRL4      0x23
#define IMU_REG_OUT_X_L    0x28

/* 配置值 */
#define IMU_CTRL1_ODR_100HZ_XYZ 0x57 /**< ODR=100Hz，三轴使能 */
#define IMU_CTRL4_BDU_FS2G      0x80 /**< BDU=1，±2g */
#define IMU_AUTO_INCREMENT      0x80 /**< 寄存器地址自增位（读多字节时置 1） */

/** 三轴原始数据字节数（X/Y/Z 各 2 字节，低字节在前） */
#define IMU_OUT_LEN 6

/** 是否已初始化 */
static bool s_initialized = false;

/* I2C 器件句柄（挂在 bsp 共享传感器总线上） */
static i2c_master_dev_handle_t s_dev = NULL;

/* 上次朝向（已不再用于平放保持——平放显式返回 FLAT；保留仅供诊断） */
static imu_sc7a20h_orientation_t s_last_orient = IMU_SC7A20H_ORIENT_LANDSCAPE;

/**
 * @brief 写单个寄存器
 */
static esp_err_t imu_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), IMU_I2C_TIMEOUT_MS);
}

/**
 * @brief 读单个寄存器
 */
static esp_err_t imu_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, IMU_I2C_TIMEOUT_MS);
}

/**
 * @brief 读三轴原始数据（地址自增，6 字节）
 */
static esp_err_t imu_read_xyz(int16_t *x, int16_t *y, int16_t *z)
{
    uint8_t reg = IMU_REG_OUT_X_L | IMU_AUTO_INCREMENT;
    uint8_t buf[IMU_OUT_LEN] = { 0 };
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, IMU_OUT_LEN,
                                                IMU_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    /* 低字节在前，16bit 补码 */
    *x = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
    *y = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
    *z = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
    return ESP_OK;
}

/**
 * @brief 由重力主导轴与符号判定朝向（重力主导轴法）
 *
 * 取绝对值最大的轴为主导轴：
 *   · |Z| 最大 -> 设备近乎平放，无法区分横竖 -> 显式返回 FLAT，
 *     由调用方决定"保持现状"（2026-10 修复：此前平放返回的是驱动初始
 *     默认值 PORTRAIT，导致"平放开机 + 自动旋转开启"时被误判为竖屏，
 *     开机约 1.2s 后自动旋转 90° 并触发全 UI 重建——实机 UI 冻结的
 *     触发路径之一）；
 *   · 否则按主导轴与符号映射到四种朝向。
 *
 * 符号到朝向的映射与屏幕/芯片安装方向强相关，若反向请按实际装配调整
 * （见文件头「上板调试要点」）。
 */
static void classify_orientation(int16_t x, int16_t y, int16_t z,
                                 imu_sc7a20h_orientation_t *out)
{
    int16_t ax = abs(x), ay = abs(y), az = abs(z);

    if (az >= ax && az >= ay) {
        /* 平放：显式告知调用方"姿态不确定"，不替它做任何默认假设 */
        *out = IMU_SC7A20H_ORIENT_FLAT;
        return;
    }

    if (ax >= ay) {
        /* 重力主要落在 X 轴 -> 横屏 */
        *out = (x > 0) ? IMU_SC7A20H_ORIENT_LANDSCAPE : IMU_SC7A20H_ORIENT_LANDSCAPE_INV;
    } else {
        /* 重力主要落在 Y 轴 -> 竖屏 */
        *out = (y > 0) ? IMU_SC7A20H_ORIENT_PORTRAIT : IMU_SC7A20H_ORIENT_PORTRAIT_INV;
    }
}

/**
 * @brief 卸载 SC7A20H 器件句柄（初始化失败回滚用）
 *
 * i2c_master_bus_add_device()（新版 esp_driver_i2c）在总线上**注册**一个器件
 * 句柄（占一条地址映射），而 i2c_master_bus_rm_device() 把它从总线上**注销**
 * 并释放句柄。两者成对：add 之后若 init 中途失败却不 rm，重试 init 会再次
 * add 同一地址——旧句柄泄漏（总线器件表不断增长），最终 add 返回错误或耗尽
 * 资源。故任何 add 成功之后的失败路径都必须 rm 并把句柄置 NULL。
 */
static void imu_release_dev(void)
{
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
}

esp_err_t imu_sc7a20h_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_ERR_INVALID_STATE;
    }

    /* 防御：若上次 init 中途失败留下残留句柄，先注销再重建
     * （正常路径 imu_release_dev() 已保证 s_dev == NULL） */
    imu_release_dev();

    /* ---- ① 取共享传感器 I2C 总线（与 AHT20 共用同一总线控制器） ---- */
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = board_i2c_sensor_bus_get(&bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "获取传感器 I2C 总线失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ② 挂载 SC7A20H 器件（7 位地址） ---- */
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMU_I2C_ADDR,
        .scl_speed_hz = IMU_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "添加 SC7A20H 器件失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- ③ 器件 ID 自检（仅告警，不致命） ---- */
    uint8_t who = 0;
    err = imu_read_reg(IMU_REG_WHO_AM_I, &who);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读 SC7A20H ID 失败: %s", esp_err_to_name(err));
        goto fail_dev;
    }
    if (who != IMU_WHO_AM_I_EXPECT) {
        ESP_LOGW(TAG, "SC7A20H ID = 0x%02X（预期 0x%02X），批次可能不同，继续尝试初始化", who,
                 IMU_WHO_AM_I_EXPECT);
    }

    /* ---- ④ 配置量程与输出数据率 ----
     * CTRL_REG1 = ODR 100Hz + 三轴使能；CTRL_REG4 = BDU + ±2g。
     * ±2g 量程下重力分量分辨率最高，适合姿态/朝向检测。 */
    err = imu_write_reg(IMU_REG_CTRL4, IMU_CTRL4_BDU_FS2G);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 CTRL_REG4 失败: %s", esp_err_to_name(err));
        goto fail_dev;
    }
    err = imu_write_reg(IMU_REG_CTRL1, IMU_CTRL1_ODR_100HZ_XYZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 CTRL_REG1 失败: %s", esp_err_to_name(err));
        goto fail_dev;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "初始化完成（I2C 地址 0x%02X，ID=0x%02X，±2g @100Hz）", IMU_I2C_ADDR, who);
    return ESP_OK;

fail_dev:
    /* 失败回滚：注销器件句柄，保证重试 init 不泄漏（见 imu_release_dev 注释） */
    imu_release_dev();
    return err;
}

esp_err_t imu_sc7a20h_get_orientation(imu_sc7a20h_orientation_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int16_t x = 0, y = 0, z = 0;
    esp_err_t err = imu_read_xyz(&x, &y, &z);
    if (err != ESP_OK) {
        return err;
    }

    classify_orientation(x, y, z, out);
    s_last_orient = *out;
    return ESP_OK;
}
