/**
 * @file board.c
 * @brief 板级支持层实现
 *
 * 只做与具体板卡相关、且与上层业务无关的初始化工作。
 */
#include "board.h"

#include "board_pins.h"

#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"

/** 日志标签 */
static const char *TAG = "board";

#define BOARD_NAME "XT60 Power Meter"    /**< 板卡名称 */
#define BOARD_REV  "上板 V2 / 下板 V1.0" /**< 板卡版本（以原理图版本为准） */

/* 传感器 I2C 总线参数（AHT20 + SC7A20H 共享，见 board_pins.h） */
#define BOARD_SENSOR_I2C_PORT     I2C_NUM_1
#define BOARD_SENSOR_I2C_FREQ_HZ  100000 /* 100kHz，两类传感器都足够
                                         * （新版 i2c_master 的速率由各器件在
                                         *  add_device 时用 scl_speed_hz 各自指定，
                                         *  本宏仅作参考值，不参与总线配置） */

/** 传感器 I2C 总线句柄（create-or-get，见 board_i2c_sensor_bus_get） */
static i2c_master_bus_handle_t s_sensor_bus = NULL;

/**
 * @brief 板级初始化
 *
 * 应在 app_main() 中最先调用，早于任何外设驱动初始化。
 * 当前完成：打印板卡标识与芯片信息；后续在此加入共享总线（SPI/I2C）初始化。
 *
 * @return ESP_OK 成功
 */
esp_err_t board_init(void)
{
    /* 读取芯片信息，便于从日志确认实际运行环境（型号/核数/版本）。
     * esp_chip_info 把芯片型号、核数、版本等一次性填进结构体，无需访问外设。 */
    esp_chip_info_t chip = { 0 };
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "%s (%s)", BOARD_NAME, BOARD_REV);

    /* revision 编码为「主版本 × 100 + 次版本」，故拆成 v主.次 显示 */
    ESP_LOGI(TAG, "MCU: ESP32-S3 rev v%d.%d, %d core(s), ESP-IDF %s", chip.revision / 100,
             chip.revision % 100, chip.cores, esp_get_idf_version());

    return ESP_OK;
}

esp_err_t board_i2c_sensor_bus_get(i2c_master_bus_handle_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 已创建过：直接复用同一句柄，保证多器件共享同一总线控制器 */
    if (s_sensor_bus != NULL) {
        *out = s_sensor_bus;
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_SENSOR_I2C_PORT,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* 板上已有 2.2k 上拉，内部上拉兜底 */
    };

    /* i2c_new_master_bus：创建一条 I2C 主机总线（新版 esp_driver_i2c 的入口）。
     * 关键字段：
     *   i2c_port             ：用哪个 I2C 外设（I2C_NUM_1 即 I2C1）；
     *   sda/scl_io_num       ：数据/时钟引脚；
     *   clk_source           ：时钟源，DEFAULT 由驱动自动选；
     *   glitch_ignore_cnt    ：滤除窄于该采样数的毛刺（7 为官方推荐值）；
     *   enable_internal_pullup：内部上拉开关（板上已有外部上拉，这里仅兜底）。 */
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_sensor_bus);
    if (err != ESP_OK) {
        s_sensor_bus = NULL;
        ESP_LOGE(TAG, "传感器 I2C 总线创建失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "传感器 I2C 总线就绪（I2C%d，SCL=IO%d SDA=IO%d）", BOARD_SENSOR_I2C_PORT,
             BOARD_PIN_I2C_SCL, BOARD_PIN_I2C_SDA);

    *out = s_sensor_bus;
    return ESP_OK;
}
