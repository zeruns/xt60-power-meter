/**
 * @file app_tasks.c
 * @brief 应用任务的创建与运行状态巡检
 */
#include "app_tasks.h"

#include <math.h> /* isfinite：判断 SOC 温度是否有效（NaN=未就绪） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ads131m02.h"
#include "data_store.h"
#include "lvgl_port.h"
#include "power_measure.h"
#include "settings.h"
#include "system_state.h"
#include "ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ota_update.h"
#include "web_console.h"
#include "wifi_manager.h"

/** 日志标签 */
static const char *TAG = "app_task";

/* 巡检任务的参数（周期 / 栈 / 优先级）
 *
 * ⚠ 本文件不是配置点：这三个值全部来自 Kconfig
 *   （main/Kconfig.projbuild 的 APP_SYS_MONITOR_* 项），
 *   下面直接以 CONFIG_ 宏使用，**不设本地默认值**。
 *
 * 原因：曾经用过 "#ifdef CONFIG_xxx / #else 本地默认值 / #endif" 的写法，
 *       但 Kconfig 已定义该项，sdkconfig 中必然存在对应宏，于是
 *       #else 分支成为死代码——改它无效且编译不报错。
 *
 * 修改方法：idf.py menuconfig → 组件配置 → XT60 功率计应用配置
 */

/** 巡检任务句柄，用于查询它的栈使用情况 */
static TaskHandle_t s_monitor_task = NULL;

/**
 * @brief 系统巡检任务（开发者观察窗口）
 *
 * 周期性打印运行状态、内存水位、栈水位与测量数据更新计数。
 * 联调阶段这个任务很有价值，具体表现在：
 *   · heap / min_heap 可以看出是否存在内存泄漏；
 *   · 栈水位（HighWaterMark）可以把"栈该给多大"从猜测变成实测；
 *   · update_count 能立刻判断测量核是否还在正常产出数据。
 *
 * 优先级设为最低，保证它绝不会影响业务任务。
 */
static void task_sys_monitor(void *arg)
{
    (void)arg;

    for (;;) {
        /* --- 系统状态与故障 --- */
        char fault_text[96];
        system_state_fault_to_str(system_state_get_fault_mask(), fault_text, sizeof(fault_text));

        /* --- 内存水位 ---
         * heap = 当前可用堆；min_heap = 历史最低值。
         * 若 min_heap 持续下降，说明有内存泄漏。 */
        ESP_LOGI(TAG, "状态=%s 故障=%s 核=%d 堆=%u 最低堆=%u",
                 system_state_name(system_state_get()), fault_text, xPortGetCoreID(),
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());

        /* --- 内部 RAM 水位（LVGL 改用系统堆后：小块分配仍优先内部 RAM，
         *     此值可反映"内部 RAM 是否吃紧"；历史最低持续下降同样提示泄漏） --- */
        ESP_LOGI(TAG, "内部 RAM 空闲 %u 字节（历史最低 %u）",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

        /* --- 栈水位 ---
         * ⚠ 三个容易踩的坑（都是 ESP-IDF 与上游 FreeRTOS 的差异）：
         *   1. uxTaskGetStackHighWaterMark() 返回单位是「字节」，
         *      而 FreeRTOS 官方文档写的是「字」——IDF 改了语义；
         *   2. ESP-IDF 里 StackType_t 被定义成 uint8_t（portSTACK_TYPE），
         *      所以 sizeof(StackType_t) == 1，
         *      不能再按上游习惯写 stack * sizeof(StackType_t) 去凑字节；
         *   3. 同理，xTaskCreate() 的栈深度参数单位也是「字节」而不是「字」，
         *      所以下面 CONFIG_APP_SYS_MONITOR_STACK = 3072 表示 3072 字节。
         * 该值是"任务历史上栈剩余空间的最小值"，越小说明越接近栈溢出，
         * 经验上应保留 25%~50% 余量。 */
        UBaseType_t stack_free_bytes = uxTaskGetStackHighWaterMark(s_monitor_task);
        ESP_LOGI(TAG, "巡检任务栈：剩余 %u 字节 / 共 %d 字节", (unsigned)stack_free_bytes,
                 CONFIG_APP_SYS_MONITOR_STACK);

        /* --- LVGL 任务栈水位（关键观测项）---
         * 2026-10 实机曾因 tiny_ttf 首次光栅化新字号/新字形（设置页）导致
         * taskLVGL 栈溢出复位（详见 lvgl_port.c 的 LVGL_PORT_TASK_STACK 注释）。
         * 该任务由官方 esp_lvgl_port 创建（句柄不对外），因此按任务名查找
         * 并常驻打印其剩余栈——今后新增字号/大批新汉字后，复查本值即可
         * 提前发现栈吃紧（余量建议不低于 25%，当前栈 12288 → 不低于约 3KB）。
         *
         * 同时打印"冻结三角"诊断（2026-10 实机 UI 冻结排查）：
         *   flush 增量 = 渲染管线是否在出帧（LV_EVENT_FLUSH_FINISH 计数差）；
         *   data  增量 = 测量核是否在产出快照（data_store 更新计数差）；
         *   LVGL 状态 = taskLVGL 的调度状态（0运行/1就绪/2阻塞/3挂起）；
         *   锁探测   = 1ms 试探取 LVGL 锁（taskLVGL 是否长期持锁不放）。
         * 屏幕冻结时的判读：
         *   flush=0 且 LVGL=0 → taskLVGL 在持锁忙等（QSPI 传输完成丢失）；
         *   flush=0 且 LVGL=2 且锁可得 → 任务睡死/时间冻结（tick/timer 异常）；
         *   flush>0 且 data=0 → 渲染活着，是测量核停了（"数据冻结"非 UI 冻结）。 */
        {
            static TaskHandle_t s_lvgl_task = NULL;
            static uint32_t s_prev_flush = 0;
            static uint32_t s_prev_data = 0;
            static uint32_t s_prev_drdy = 0;
            if (s_lvgl_task == NULL) {
                /* 首个巡检周期时 LVGL 任务可能尚未创建，之后周期会自动补上 */
                s_lvgl_task = xTaskGetHandle("taskLVGL");
            }
            if (s_lvgl_task != NULL) {
                const uint32_t flush_now = lvgl_port_flush_count();
                const uint32_t data_now = data_store_update_count();
                const uint32_t drdy_now = ads131m02_drdy_count();
                /* eTaskGetState：eRunning=0 / eReady=1 / eBlocked=2 / eSuspended=3 */
                int state = (int)eTaskGetState(s_lvgl_task);
                bool lock_ok = lvgl_port_lock(1);
                if (lock_ok) {
                    lvgl_port_unlock();
                }
                /* UI 任务心跳间隔（越大说明 UI 任务越久没跑完一轮） */
                const uint32_t ui_age_ms =
                    (uint32_t)((esp_timer_get_time() - ui_get_last_loop_us()) / 1000);
                ESP_LOGI(TAG,
                         "冻结诊断: flush+%u data+%u drdy+%u(%.0fHz) LVGL态=%d 锁=%s UI心跳龄=%ums | LVGL栈剩 %u/%d",
                         (unsigned)(flush_now - s_prev_flush),
                         (unsigned)(data_now - s_prev_data),
                         (unsigned)(drdy_now - s_prev_drdy),
                         (drdy_now - s_prev_drdy) / 5.0,
                         state, lock_ok ? "可得" : "被占", (unsigned)ui_age_ms,
                         (unsigned)uxTaskGetStackHighWaterMark(s_lvgl_task), 16384);
                /* 渲染停滞告警：flush 在 5s 内零增长 = taskLVGL 出不了帧 */
                if (flush_now == s_prev_flush) {
                    ESP_LOGE(TAG, "!! 渲染停滞：5s 内 flush 零增长（LVGL态=%d 锁=%s UI心跳龄=%ums）",
                             state, lock_ok ? "可得" : "被占", (unsigned)ui_age_ms);
                }
                s_prev_flush = flush_now;
                s_prev_data = data_now;
                s_prev_drdy = drdy_now;
            }
        }

        /* --- 引擎统计（M5 验收取数口径：drop/超时/δ，见 ads131m02.h）---
         * 判读：drop 与 超时 应为 0 或极小；δ max 远小于 250µs 转换周期；
         * crc_fail 持续增长提示帧错位（需重同步）。 */
        {
            ads131m02_stats_t est;
            ads131m02_get_stats(&est);
            ESP_LOGI(TAG, "引擎: kick=%u drop=%u read_ok=%u crc_fail=%u 超时=%u δ min/avg/max=%u/%u/%uµs",
                     (unsigned)est.edge_ok, (unsigned)est.edge_busy, (unsigned)est.read_ok,
                     (unsigned)est.crc_fail, (unsigned)est.read_timeout, (unsigned)est.lat_min_us,
                     (unsigned)est.lat_avg_us, (unsigned)est.lat_max_us);
        }

        /* --- 测量数据链路 ---
         * data_store 尚无数据时返回 ESP_ERR_INVALID_STATE，
         * 这是"测量核还没开始工作"的正常表现，不算故障。 */
        data_store_snapshot_t snapshot;
        if (data_store_get(&snapshot) == ESP_OK) {
            /* SOC 结温用 %.1f 输出；NaN（传感器未就绪）时显示 --，便于长跑观测
             * 主控发热情况（v0.5.1 新增，来源为芯片内部温度传感器）。 */
            char soc[16];
            if (isfinite(snapshot.soc_temp_c)) {
                snprintf(soc, sizeof(soc), "%.1f", (double)snapshot.soc_temp_c);
            } else {
                snprintf(soc, sizeof(soc), "--");
            }
            ESP_LOGI(TAG, "测量: %.3f V / %.3f A / %.3f W  SOC=%s℃（累计更新 %u 次）",
                     snapshot.voltage_v, snapshot.current_a, snapshot.power_w, soc,
                     (unsigned)data_store_update_count());
        } else {
            ESP_LOGD(TAG, "测量数据尚未就绪（data_store 更新计数 %u）",
                     (unsigned)data_store_update_count());
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_SYS_MONITOR_PERIOD_MS));
    }
}

/**
 * @brief web_console 参数下发回调：把网页键值对写进 settings（v0.5.0 联网数据版）
 *
 * 支持键：alarm_v_en/alarm_i_en/alarm_p_en（0/1）、alarm_v_limit/alarm_i_limit/
 * alarm_p_limit（float）、buzzer_volume/lcd_brightness/display_rate_hz。
 * 在 httpd 任务执行，只做内存缓存写 + 防抖落盘（settings_set_* 内部已防抖）。
 */
static esp_err_t web_config_handler(const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_alarm_t alm;
    settings_t cfg;

    /* ---- F7 配网 API：wifi_ssid / wifi_pass 成对下发 → STA 连接 ----
     * 两键到齐才发起连接（JSON 体内先后顺序不限）；空密码网络 pass 允许为空
     * 字符串。wifi_manager_connect() 内部等待连接结果（最长约 15s），会占用
     * httpd 任务一段时间——配网页单次提交场景可接受，且连接成功后设备将离开
     * AP 模式，本次连接本身也即将断开。凭据由 wifi_manager 持久化（NVS wifi）。 */
    {
        static char s_pending_ssid[33] = "";
        static char s_pending_pass[65] = "";
        static bool s_pass_seen = false; /* 区分「没收到 pass」与「pass 为空密码」 */
        bool wifi_touched = false;
        if (strcmp(key, "wifi_ssid") == 0) {
            snprintf(s_pending_ssid, sizeof(s_pending_ssid), "%s", value);
            wifi_touched = true;
        } else if (strcmp(key, "wifi_pass") == 0) {
            snprintf(s_pending_pass, sizeof(s_pending_pass), "%s", value);
            s_pass_seen = true;
            wifi_touched = true;
        }
        /* 两键到齐（顺序不限）即发起连接 */
        if (wifi_touched && s_pending_ssid[0] != '\0' && s_pass_seen) {
            ESP_LOGI(TAG, "配网下发：%s，发起 STA 连接", s_pending_ssid);
            esp_err_t werr = wifi_manager_connect(s_pending_ssid, s_pending_pass);
            s_pending_ssid[0] = '\0';
            s_pending_pass[0] = '\0';
            s_pass_seen = false;
            return werr;
        }
        if (wifi_touched) {
            return ESP_OK; /* 只到齐一半，等另一半 */
        }
    }

    if (strcmp(key, "alarm_v_en") == 0 || strcmp(key, "alarm_i_en") == 0 ||
        strcmp(key, "alarm_p_en") == 0) {
        if (settings_get_alarm(&alm) != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        uint8_t on = (uint8_t)atoi(value);
        if (strcmp(key, "alarm_v_en") == 0) {
            alm.v_en = on;
        } else if (strcmp(key, "alarm_i_en") == 0) {
            alm.i_en = on;
        } else {
            alm.p_en = on;
        }
        return settings_set_alarm(&alm);
    }
    if (strcmp(key, "alarm_v_limit") == 0 || strcmp(key, "alarm_i_limit") == 0 ||
        strcmp(key, "alarm_p_limit") == 0) {
        if (settings_get_alarm(&alm) != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        float v = strtof(value, NULL);
        if (strcmp(key, "alarm_v_limit") == 0) {
            alm.v_limit = v;
        } else if (strcmp(key, "alarm_i_limit") == 0) {
            alm.i_limit = v;
        } else {
            alm.p_limit = v;
        }
        return settings_set_alarm(&alm);
    }
    if (strcmp(key, "buzzer_volume") == 0) {
        return settings_set_buzzer_volume((uint8_t)atoi(value));
    }
    if (strcmp(key, "lcd_brightness") == 0) {
        return settings_set_lcd_brightness((uint8_t)atoi(value));
    }
    if (strcmp(key, "display_rate_hz") == 0) {
        return settings_set_display_rate_hz((uint8_t)atoi(value));
    }
    (void)cfg;
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief 启动联网数据服务（v0.5.0）：WiFi 状态机 + Web 控制台 + OTA 回滚检查
 *
 * 全部非关键：任一步失败只记录日志降级运行，测量/显示不受影响。
 * WiFi 启动后（含 AP 配网模式）即可访问 Web 控制台；HTTP 服务本身
 * 不依赖 STA 已连接——AP 模式下手机直连热点也能配网/读数。
 */
static void net_services_start(void)
{
    /* OTA 回滚检查（最早做：新固件启动失败自动回滚，见 ota_update.h） */
    esp_err_t oerr = ota_update_rollback_if_pending();
    if (oerr != ESP_OK) {
        ESP_LOGW(TAG, "OTA 回滚检查异常: %s", esp_err_to_name(oerr));
    } else {
        ESP_LOGI(TAG, "OTA 回滚检查完成，当前版本 %s", ota_update_get_current_version());
    }

    /* WiFi 管理：有凭据走 STA，无凭据进 AP 配网模式 */
    esp_err_t werr = wifi_manager_start();
    if (werr != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 启动失败（降级为离线运行）: %s", esp_err_to_name(werr));
        return;
    }

    /* 参数下发回调注入（web → settings），再拉起 Web 控制台 */
    (void)web_console_set_config_handler(web_config_handler);
    esp_err_t cerr = web_console_start();
    if (cerr != ESP_OK) {
        ESP_LOGW(TAG, "Web 控制台启动失败: %s", esp_err_to_name(cerr));
    }
}

esp_err_t app_tasks_start(void)
{
    /* ---- 测量任务（最先启动，绑 Core1）----
     * 4kSPS 由 ADS131M02 DRDY 自节奏驱动；ADC 关键故障时 init 已判失败，
     * 能走到这里说明测量链路初始化通过。启动失败不阻断 UI 与巡检，
     * 但会记录错误日志（此时通常是任务内存不足，属系统级异常）。 */
    if ((system_state_get_fault_mask() & SYSTEM_FAULT_ADC) == 0) {
        esp_err_t err = power_measure_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "测量任务启动失败: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "测量前端故障，跳过测量任务启动");
    }

    /* ---- UI 任务 ----
     * 门控条件：显示链路（lcd_nv3030b + lvgl_port）必须初始化成功。
     * 走到这里说明启动表与硬件自检都已完成（见 app_init.c），
     * 但 UI 只依赖显示链路，故仍单独检查 SYSTEM_FAULT_LCD：
     *   · lvgl_port 未初始化时调用任何 LVGL API 都是未定义行为，必须先查故障位；
     *   · 触摸故障（SYSTEM_FAULT_TOUCH）不阻止 UI 启动——界面仍可显示，只是不能触控。
     * 注意：界面本身是在 ui 任务内部构建的（不在 app_main 的栈上），原因见 ui.c。 */
    if ((system_state_get_fault_mask() & SYSTEM_FAULT_LCD) == 0) {
        esp_err_t err = ui_init();
        if (err != ESP_OK) {
            /* UI 启动失败不阻断测量等业务任务，降级继续（无界面运行） */
            ESP_LOGE(TAG, "UI 启动失败: %s -> 无界面运行", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "显示链路故障，跳过 UI 启动");
    }

    /* xTaskCreatePinnedToCore 的最后一个参数就是绑定的核编号。
     * 相比 xTaskCreate（由调度器任意分配），显式绑核才能保证
     * "测量核不被通信任务打扰"这一设计目标。 */
    BaseType_t ok =
        xTaskCreatePinnedToCore(task_sys_monitor, "sys_monitor", CONFIG_APP_SYS_MONITOR_STACK, NULL,
                                CONFIG_APP_SYS_MONITOR_PRIO, &s_monitor_task, APP_CORE_UI);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 sys_monitor 任务失败（栈 %d 字节）", CONFIG_APP_SYS_MONITOR_STACK);
        return ESP_ERR_NO_MEM;
    }

    /* ---- 联网数据服务（v0.5.0）：非关键，失败降级离线运行 ----
     * ⚠ 必须在测量与 UI 任务之后启动：WiFi 栈 + LWIP + HTTP 启动会占用大量
     * RAM，若先启动会挤掉 UI 的内存（实测 ESP_ERR_NO_MEM -> UI 无界面运行）。
     * 先让测量/UI 各自占好内存，联网最后进场。 */
    net_services_start();

    ESP_LOGI(TAG, "应用任务创建完成（巡检任务绑定 Core %d，周期 %d ms）", APP_CORE_UI,
             CONFIG_APP_SYS_MONITOR_PERIOD_MS);
    return ESP_OK;
}
