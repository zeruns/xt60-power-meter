/**
 * @file orientation.c
 * @brief 屏幕重力自动旋转服务实现（见 orientation.h）
 *
 * 设计要点：
 *   · 独立低优先级任务（Core0，栈 3KB，周期 200ms），I2C 读取阻塞 ≤ 几 ms；
 *   · 去抖：连续 ORIENT_CONFIRM_N 次读到同一姿态才切换（≈1.2s 稳定），
 *     防止转动瞬间抖动导致来回翻转；平放时驱动显式报告 FLAT，本服务
 *     直接跳过（保持当前方向），不会用任何默认朝向去猜测；
 *   · 只在角度真正变化时调 lvgl_port_set_rotation_angle()，避免无谓刷屏；
 *   · 自动开关不持久化在本组件——由 settings 统一保存，本组件只记运行态。
 */
#include "orientation.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu_sc7a20h.h"
#include "lvgl_port.h"
#include "settings.h"

static const char *TAG = "orientation";

#define ORIENT_TASK_STACK 3072
#define ORIENT_TASK_PRIO 4
#define ORIENT_PERIOD_MS 200
#define ORIENT_CONFIRM_N 6 /**< 连续同向次数确认（6×200ms ≈ 1.2s） */
#define ORIENT_INVALID_ANG 0xFFFFu

static bool s_inited = false;
static volatile bool s_auto = false;
static TaskHandle_t s_task = NULL;

/**
 * IMU 姿态 → 屏幕角度（以默认横屏为 0°，顺时针）。
 *
 * ⚠ 装配相关：SC7A20H 的轴方向与屏幕的对应关系取决于焊接方向。
 *   当前为自然约定（LANDSCAPE=0°、PORTRAIT=90°）；若上板发现自动旋转
 *   横竖对调或镜像，**只改这张表**（驱动 classify_orientation 的符号
 *   判定也可能需要一起翻转，见 imu_sc7a20h.c 文件头调试说明）。
 */
static uint16_t imu_orient_to_angle(imu_sc7a20h_orientation_t o)
{
    switch (o) {
    case IMU_SC7A20H_ORIENT_LANDSCAPE:
        return 0;
    case IMU_SC7A20H_ORIENT_PORTRAIT:
        return 90;
    case IMU_SC7A20H_ORIENT_LANDSCAPE_INV:
        return 180;
    case IMU_SC7A20H_ORIENT_PORTRAIT_INV:
        return 270;
    default:
        return ORIENT_INVALID_ANG;
    }
}

/** 姿态有效且与当前显示角度不同时切换显示方向 */
static void apply_orientation(imu_sc7a20h_orientation_t cur)
{
    uint16_t target = imu_orient_to_angle(cur);
    if (target == ORIENT_INVALID_ANG) {
        return; /* FLAT 等无效姿态：保持现状 */
    }
    if (target != lvgl_port_get_rotation_angle()) {
        esp_err_t err = lvgl_port_set_rotation_angle(target);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "重力旋转至 %u°", (unsigned)target);
        } else {
            ESP_LOGW(TAG, "旋转失败: %s", esp_err_to_name(err));
        }
    }
}

static void task_orientation(void *arg)
{
    (void)arg;

    imu_sc7a20h_orientation_t pending = IMU_SC7A20H_ORIENT_LANDSCAPE;
    int confirm = 0;

    for (;;) {
        /* 200ms 周期；orientation_set_auto(true) 会发通知提前唤醒，
         * 用于"开启自动旋转后立即校正一次"（无需等防抖确认）。 */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ORIENT_PERIOD_MS));

        if (!s_auto) {
            confirm = 0;
            continue;
        }

        imu_sc7a20h_orientation_t cur;
        if (imu_sc7a20h_get_orientation(&cur) != ESP_OK) {
            continue;
        }

        if (cur == IMU_SC7A20H_ORIENT_FLAT) {
            /* 平放：无法区分横竖，保持当前方向不动，防抖计数重新起算 */
            confirm = 0;
            continue;
        }

        if (notified) {
            /* 用户主动开启且姿态明确：立即校正一次，防抖计数从此姿态起算 */
            pending = cur;
            confirm = ORIENT_CONFIRM_N;
            apply_orientation(cur);
            continue;
        }

        if (cur != pending) {
            pending = cur;
            confirm = 1;
            continue;
        }
        if (++confirm < ORIENT_CONFIRM_N) {
            continue;
        }

        /* 姿态稳定：仅当角度与当前显示方向不同时切换 */
        apply_orientation(cur);
    }
}

esp_err_t orientation_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(task_orientation, "orientation",
                                           ORIENT_TASK_STACK, NULL, ORIENT_TASK_PRIO,
                                           &s_task, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    ESP_LOGI(TAG, "方向监测任务已启动（%dms 周期，%d 次确认）", ORIENT_PERIOD_MS,
             ORIENT_CONFIRM_N);
    return ESP_OK;
}

void orientation_set_auto(bool enable)
{
    s_auto = enable;
    ESP_LOGI(TAG, "重力自动旋转：%s", enable ? "开" : "关");
    if (enable && s_task != NULL) {
        /* 立即唤醒监测任务按当前姿态校正一次（开启即生效，不等 1.2s 防抖） */
        xTaskNotifyGive(s_task);
    }
}

bool orientation_get_auto(void)
{
    return s_auto;
}

esp_err_t orientation_boot_restore(void)
{
    /* 先恢复上次保存的手动角度（自动关闭时的初始朝向；默认 0° 横屏） */
    bool auto_on = false;
    settings_t cfg;
    if (settings_get(&cfg) == ESP_OK) {
        if (lvgl_port_get_rotation_angle() != cfg.orientation_angle) {
            (void)lvgl_port_set_rotation_angle((uint16_t)cfg.orientation_angle);
        }
        auto_on = (cfg.orientation_auto != 0);
        ESP_LOGI(TAG, "方向已恢复：自动=%u 角度=%u°", (unsigned)cfg.orientation_auto,
                 (unsigned)cfg.orientation_angle);
    }

    /* 先建监测任务，再开自动：set_auto 的通知才能被任务收到，
     * 开机即按当前握持姿态校正一次（否则要等一个防抖周期）。 */
    esp_err_t err = orientation_init();
    if (err != ESP_OK) {
        return err;
    }
    if (auto_on) {
        orientation_set_auto(true);
    }
    return ESP_OK;
}
