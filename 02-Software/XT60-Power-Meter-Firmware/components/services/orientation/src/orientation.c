/**
 * @file orientation.c
 * @brief 屏幕重力自动旋转服务实现（见 orientation.h）
 *
 * 职责：周期读 SC7A20H 加速度计判定的握持姿态，把姿态映射成屏幕角度
 * （0°/90°/180°/270°，顺时针，以默认横屏为 0°），在姿态稳定后驱动
 * LVGL 显示与触摸同步旋转到目标角度。
 *
 * 设计要点：
 *   · 独立低优先级任务（Core0，栈 3KB，周期 200ms），I2C 读取阻塞 ≤ 几 ms；
 *   · 去抖：连续 ORIENT_CONFIRM_N 次读到同一姿态才切换（≈1.2s 稳定），
 *     防止转动瞬间抖动导致来回翻转；平放时驱动显式报告 FLAT，本服务
 *     直接跳过（保持当前方向），不会用任何默认朝向去猜测；
 *   · 只在角度真正变化时调 lvgl_port_set_rotation_angle()，避免无谓刷屏；
 *   · 自动开关不持久化在本组件——由 settings 统一保存，本组件只记运行态。
 *
 * 关于"触摸也要跟着转"：lvgl_port_set_rotation_angle() 内部在转显示的同时
 * 会重建触摸坐标的旋转矩阵。注意触摸映射必须做**反向复合**旋转——显示把
 * 内容顺时针转 90°，触摸点反过来要按逆时针 90° 从屏幕坐标映射回内容坐标，
 * 方向搞反就会出现"点 A 响应在 B"的镜像错位（本项目踩过这个坑）。
 * 该矩阵的细节在 lvgl_port 组件内，本服务只负责给它传角度。
 */
#include "orientation.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu_sc7a20h.h"
#include "lvgl_port.h"
#include "settings.h"

/** 日志标签（ESP_LOGx 宏第一个参数，用于按模块过滤日志） */
static const char *TAG = "orientation";

#define ORIENT_TASK_STACK 3072      /**< 监测任务栈（字节）：只需放姿态变量与日志缓冲 */
#define ORIENT_TASK_PRIO 4          /**< 任务优先级（数值越大越高），低于 UI 任务 */
#define ORIENT_PERIOD_MS 200        /**< 姿态采样周期（毫秒） */
#define ORIENT_CONFIRM_N 6 /**< 连续同向次数确认（6×200ms ≈ 1.2s） */
#define ORIENT_INVALID_ANG 0xFFFFu  /**< 哨兵值：该姿态无法映射为屏幕角度（如 FLAT） */

static bool s_inited = false;            /**< 是否已创建监测任务（防重复创建） */
static volatile bool s_auto = false;     /**< 自动旋转运行态开关；volatile：任务与调用方都会读写 */
static TaskHandle_t s_task = NULL;       /**< 监测任务句柄，set_auto 用来发通知唤醒它 */

/**
 * IMU 姿态 → 屏幕角度（以默认横屏为 0°，顺时针）。
 *
 * ⚠ 装配相关：SC7A20H 的轴方向与屏幕的对应关系取决于焊接方向。
 *   2026-10 实机标定：竖屏两向原表反了（PORTRAIT 应对 270°、
 *   PORTRAIT_INV 应对 90°），已按实测对调；横屏两向不变。
 *   换板/换 IMU 贴片方向时**只改这张表**（驱动 classify_orientation 的
 *   符号判定也可能需要一起翻转，见 imu_sc7a20h.c 文件头调试说明）。
 *
 * @param o 驱动上报的姿态枚举
 * @return 目标屏幕角度（0/90/180/270）；FLAT 等无法映射时返回 ORIENT_INVALID_ANG
 */
static uint16_t imu_orient_to_angle(imu_sc7a20h_orientation_t o)
{
    switch (o) {
    case IMU_SC7A20H_ORIENT_LANDSCAPE:
        return 0;
    case IMU_SC7A20H_ORIENT_PORTRAIT:
        return 270;
    case IMU_SC7A20H_ORIENT_LANDSCAPE_INV:
        return 180;
    case IMU_SC7A20H_ORIENT_PORTRAIT_INV:
        return 90;
    default:
        return ORIENT_INVALID_ANG;
    }
}

/**
 * @brief 姿态有效且与当前显示角度不同时切换显示方向
 *
 * 两道过滤：① 姿态映射不出角度（FLAT）直接放弃；② 目标角度与当前显示
 * 相同也放弃。第二道很重要——重复调用旋转会触发整屏重绘，白白费帧率。
 *
 * @param cur 驱动上报的当前姿态
 */
static void apply_orientation(imu_sc7a20h_orientation_t cur)
{
    uint16_t target = imu_orient_to_angle(cur);
    if (target == ORIENT_INVALID_ANG) {
        return; /* FLAT 等无效姿态：保持现状 */
    }
    if (target != lvgl_port_get_rotation_angle()) {
        esp_err_t err = lvgl_port_set_rotation_angle(target);
        /* ^ 同步旋转 LVGL 显示与触摸映射（触摸侧做反向复合，见文件头） */
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "重力旋转至 %u°", (unsigned)target);
        } else {
            ESP_LOGW(TAG, "旋转失败: %s", esp_err_to_name(err));
        }
    }
}

/**
 * @brief 姿态监测任务主体（FreeRTOS 任务函数，永不返回）
 *
 * 每轮循环：等下一个周期 → 读姿态 → 平放跳过 → 防抖计数 → 稳定后应用。
 * 防抖是一个"计数器式确认"：姿态一变就把计数清 1 重新起算，连续
 * ORIENT_CONFIRM_N 次同向才认为用户真的转了屏，扛住转动途中的抖动读数。
 *
 * @param arg 未使用（FreeRTOS 要求任务函数签名固定为 void(*)(void*)）
 */
static void task_orientation(void *arg)
{
    (void)arg;

    imu_sc7a20h_orientation_t pending = IMU_SC7A20H_ORIENT_LANDSCAPE; /**< 防抖计数对应的候选姿态 */
    int confirm = 0; /**< 候选姿态已连续确认的次数 */

    for (;;) {
        /* 200ms 周期；orientation_set_auto(true) 会发通知提前唤醒，
         * 用于"开启自动旋转后立即校正一次"（无需等防抖确认）。 */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ORIENT_PERIOD_MS));
        /* ^ FreeRTOS 任务通知：最多阻塞等待 200ms，期间若有人发过通知
         *   （xTaskNotifyGive）则立即返回非 0 的通知计数并清零，否则超时返回 0。
         *   比自建信号量更轻量，正好用来做"周期节拍 + 可被外部提前唤醒"。 */

        if (!s_auto) {
            confirm = 0; /* 自动旋转已关：丢弃防抖进度，下轮再看 */
            continue;
        }

        imu_sc7a20h_orientation_t cur;
        if (imu_sc7a20h_get_orientation(&cur) != ESP_OK) {
            continue; /* I2C 读失败（瞬时干扰）：本轮作废，下周期重试 */
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
            /* 候选姿态刚变：计数清 1 重新起算，等它稳定够次数 */
            pending = cur;
            confirm = 1;
            continue;
        }
        if (++confirm < ORIENT_CONFIRM_N) {
            continue; /* 同向次数还不够，继续观察 */
        }

        /* 姿态稳定：仅当角度与当前显示方向不同时切换 */
        apply_orientation(cur);
    }
}

/**
 * @brief 创建姿态监测任务（幂等：重复调用返回 ESP_ERR_INVALID_STATE）
 *
 * 任务固定跑在 Core0：让出 Core1 给显示/触摸等重负载，避免 I2C 读取
 * 偶发的几毫秒阻塞干扰 UI 刷新。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；ESP_ERR_NO_MEM 任务创建失败
 * @note 一般不直接调用本函数，开机走 orientation_boot_restore() 一次性完成恢复+启动。
 */
esp_err_t orientation_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(task_orientation, "orientation",
                                           ORIENT_TASK_STACK, NULL, ORIENT_TASK_PRIO,
                                           &s_task, 0);
    /* ^ 创建 FreeRTOS 任务并绑定到指定 CPU 核：
     *   参数依次为 任务函数、任务名、栈字节数、入参、优先级、句柄输出、目标核(0)。
     *   失败通常意味着堆内存不足，返回 pdFAIL */
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    ESP_LOGI(TAG, "方向监测任务已启动（%dms 周期，%d 次确认）", ORIENT_PERIOD_MS,
             ORIENT_CONFIRM_N);
    return ESP_OK;
}

/**
 * @brief 开关重力自动旋转（只改运行态，持久化由 settings 负责）
 *
 * 开启时顺带唤醒监测任务立即校正一次，用户勾选"自动旋转"后屏幕马上
 * 就跟着握持方向走，不用干等 1.2s 防抖周期。
 *
 * @param enable true=开启自动旋转；false=关闭（保持当前角度）
 */
void orientation_set_auto(bool enable)
{
    s_auto = enable;
    ESP_LOGI(TAG, "重力自动旋转：%s", enable ? "开" : "关");
    if (enable && s_task != NULL) {
        /* 立即唤醒监测任务按当前姿态校正一次（开启即生效，不等 1.2s 防抖） */
        xTaskNotifyGive(s_task);
        /* ^ 给监测任务发一条任务通知：使它在 ulTaskNotifyTake 处提前醒来，
         *   任务醒来后看到通知计数非 0 就走"立即校正"分支 */
    }
}

/**
 * @brief 查询当前自动旋转运行态
 * @return true=自动旋转开启；false=关闭
 */
bool orientation_get_auto(void)
{
    return s_auto;
}

/**
 * @brief 开机恢复上次的旋转配置，并启动监测任务
 *
 * 启动序列专用，做三件事：① 恢复 settings 保存的手动角度（自动关闭时的
 * 初始朝向）；② 创建监测任务；③ 若上次开着自动旋转则打开它。
 *
 * @return ESP_OK 成功；其他为 orientation_init() 的错误码
 */
esp_err_t orientation_boot_restore(void)
{
    /* 先恢复上次保存的手动角度（自动关闭时的初始朝向；默认 0° 横屏） */
    bool auto_on = false;
    settings_t cfg;
    if (settings_get(&cfg) == ESP_OK) {
        /* settings_get() 从 NVS 读出整份用户设置；读到才恢复，读不到就保持默认 */
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
