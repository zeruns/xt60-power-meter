/**
 * @file settings.c
 * @brief 配置持久化服务实现（NVS + 内存缓存 + 防抖落盘）
 *
 * 本文件是 settings.h 所述"三层结构"的具体实现：
 *   Kconfig（CONFIG_BUZZER_VOLUME_PERCENT 等）→ 只提供"出厂默认值"；
 *   NVS（Flash 上的键值库）                  → 保存用户实际调整后的值；
 *   内存缓存 s_cache                          → 提供快速读取，读取不碰 Flash。
 *
 * 数据流（写路径）：
 *   用户改设置 → settings_set_xxx() 更新内存缓存 → 标记"脏" → 重置防抖定时器
 *              → 静默 SETTINGS_SAVE_DELAY_MS 毫秒后定时器到点 → 通知防抖落盘任务
 *              → 由低优先级任务执行 persist_cache() 写 NVS
 *
 * 为什么防抖落盘要"任务化"而不是在定时器回调里直接写？
 *   esp_timer 的回调运行在共用的 esp_timer 任务上下文里，同一任务还跑着
 *   LVGL 的心跳 tick（lv_tick_inc）等回调。NVS 的 nvs_commit 可能耗时
 *   几十~几百 ms，若在回调里同步写，会连带拖住 LVGL tick，界面卡顿。
 *   所以回调只做"置脏标记 + 通知任务"两条指令级别的动作，
 *   真正的 Flash 写入由独立低优先级任务执行——定时器任务毫秒级返回。
 *
 * 为什么写入要"防抖"？
 *   NVS 位于 Flash 上，Flash 扇区有擦写寿命（约 10 万次）。
 *   用户拖动滑块时会连续产生几十次设置变化，若每次都立刻写 Flash，
 *   一次拖动就消耗几十次擦写寿命。防抖把"连续 N 次改动"合并成
 *   "最后一次改动后写一次"，从源头减少写入次数，延长 Flash 寿命。
 *   （详见 settings.h 文件头的完整说明。）
 *
 * 为什么读取可以直接读缓存？
 *   读取走内存变量、不碰 Flash，没有磨损问题，因此可以高频调用。
 *
 * 容错设计：即使 NVS 完全不可用（首次上电、分区损坏），本服务也用
 * 出厂默认值继续工作（"优雅降级"），只是无法记住用户的调整。
 */
#include "settings.h"

#include <string.h>

#include "system_state.h" /* 系统状态机：NVS 初始化失败时上报 STORAGE 故障（让故障可见） */
#include "esp_log.h"     /* ESP-IDF 日志库：ESP_LOGx 宏按级别打印带标签的日志 */
#include "esp_timer.h"   /* ESP-IDF 软件定时器：微秒精度的单次/周期回调 */
#include "freertos/FreeRTOS.h" /* FreeRTOS 核心头文件（portMUX_TYPE 等） */
#include "freertos/event_groups.h" /* FreeRTOS 事件组：跨任务的"完成"标志同步 */
#include "freertos/semphr.h"   /* FreeRTOS 信号量/互斥锁：落盘动作互斥 */
#include "freertos/task.h"   /* FreeRTOS 任务：防抖落盘任务 */
#include "nvs.h"         /* NVS 键值读写接口（nvs_open/nvs_get_xxx/nvs_set_xxx） */
#include "nvs_flash.h"   /* NVS 分区管理接口（nvs_flash_init/erase） */

/** 日志标签 */
static const char *TAG = "settings";

/** NVS 命名空间（相当于一张表名，最长 15 字符） */
#define SETTINGS_NVS_NAMESPACE     "xt60cfg"

/** 各配置项的键名（NVS 键名同样限制 15 字符） */
#define SETTINGS_KEY_BUZZER_VOLUME "buzzer_vol"
#define SETTINGS_KEY_LCD_BRIGHTNESS "lcd_bright"
#define SETTINGS_KEY_DISPLAY_RATE "rate_hz"
#define SETTINGS_KEY_ORIENT_AUTO "orient_a"
#define SETTINGS_KEY_ORIENT_ANG "orient_d"
#define SETTINGS_KEY_WAVE_SMOOTH "wave_smt"
#define SETTINGS_KEY_ALARM "alarm_cfg" /**< 告警阈值（settings_alarm_t blob） */
#define SETTINGS_KEY_WAVE_WIN "wave_win"   /**< 波形时间窗档位（B20） */
#define SETTINGS_KEY_WAVE_MODE "wave_mode" /**< 波形显示模式（B20） */
#define SETTINGS_KEY_SCR_OFF "scr_off"     /**< 自动降亮档位（F4，v0.6.2：0=永不 1=15s 2=30s 3=60s） */

/** 告警阈值出厂默认（全部关闭，阈值按前端满量程给合理上限） */
#define SETTINGS_ALARM_V_LIMIT_DEF 15.0f
#define SETTINGS_ALARM_I_LIMIT_DEF 5.0f
#define SETTINGS_ALARM_P_LIMIT_DEF 50.0f

/** 告警阈值上限（v0.6.9，与整机设计量程一致）
 *   · 电压 90V：分压链路理论 ±121V，受辅助电源最高输入 100V 限制取 90V；
 *   · 电流 70A：1mΩ/5W 分流器功率上限（XT60 额定 30A，换 XT90 底板可更高）；
 *   · 功率 6300W：90V × 70A。
 *  网页/API 与设备端 UI 都按同一组上限校验，越界整体拒绝。 */
#define SETTINGS_ALARM_V_MAX 90.0f
#define SETTINGS_ALARM_I_MAX 70.0f
#define SETTINGS_ALARM_P_MAX 6300.0f

/** 防抖时长（毫秒）：最后一次修改后静默这么久才真正写 Flash */
#define SETTINGS_SAVE_DELAY_MS     1500

/** 防抖落盘任务：栈大小（字节）与优先级
 * 优先级取低（2）：落盘不紧急，不该与 UI/测量抢时间片 */
#define SETTINGS_SAVE_TASK_STACK   2560
#define SETTINGS_SAVE_TASK_PRIO    2

/** 落盘任务的周期检查间隔（毫秒）：无通知时也醒来看看有没有没落成的盘
 * （persist_cache 失败时保留脏标记，靠这个节拍自动重试） */
#define SETTINGS_SAVE_POLL_MS      5000

/** 响度合法范围，与 buzzer 驱动保持一致 */
#define SETTINGS_VOLUME_MIN        1
#define SETTINGS_VOLUME_MAX        100

/** 亮度合法范围（下限 10：避免用户把屏幕调到全黑后无法操作） */
#define SETTINGS_BRIGHT_MIN        10
#define SETTINGS_BRIGHT_MAX        100

/**
 * @brief 检查刷新率是否为合法档位
 *
 * @param hz 待检查的刷新率
 * @return true 合法（1/5/10/20/30 之一）
 *
 * 与测量页快速菜单保持同一套档位：菜单里能选什么，这里就认什么，
 * 避免 UI 与持久化层两份白名单不一致。
 */
static bool settings_rate_valid(uint8_t hz)
{
    return hz == 1 || hz == 5 || hz == 10 || hz == 20 || hz == 30;
}

/** 内存缓存：保存当前生效的配置 */
static settings_t s_cache;

/** 告警阈值内存缓存（blob 键 "alarm_cfg" 持久化，见 settings.h） */
static settings_alarm_t s_alarm_cache;

/** 是否已初始化 */
static bool s_ready = false;

/** 是否有未落盘的改动（"脏"标记） */
static bool s_dirty = false;

/** 防抖定时器句柄 */
static esp_timer_handle_t s_save_timer = NULL;

/** 防抖落盘任务句柄与同步原语（落盘任务化，见文件头说明） */
static TaskHandle_t s_save_task = NULL;

/** 「该落盘了」通知位：防抖定时器回调 → 落盘任务
 *
 * FreeRTOS 事件组（Event Group）：一个 24/32 位的标志位集合，
 * xEventGroupSetBits 置位、xEventGroupWaitBits 可"阻塞等某位"，
 * 任务间通知用它比裸 bool + 轮询省电（等待时不占 CPU）。
 * 用事件组而不是"定时器回调直接调 persist_cache"：回调跑在共用的
 * esp_timer 任务里（与 LVGL tick 同任务），NVS 写入太慢会拖垮界面。 */
static EventGroupHandle_t s_save_events = NULL;
#define SETTINGS_EVT_DO_SAVE BIT0 /**< 事件位：防抖到点，请落盘 */

/** 保护 persist_cache() 的互斥锁
 *
 * settings_save_now()（UI 任务调用）与防抖落盘任务都可能写 NVS，
 * nvs_set_xxx/commit 交错会互相覆盖，用互斥锁把整段落盘串行化 */
static SemaphoreHandle_t s_save_mutex = NULL;

/**
 * 保护缓存与脏标记的自旋锁
 *
 * 缓存会被多个任务读写（UI 任务写、其他任务读），
 * 而脏标记会被任务与防抖定时器回调共同访问，因此需要保护。
 * 临界区里只做几个字节的赋值，开销可忽略。
 *
 * 配套 API（FreeRTOS/Xtensa 提供）：
 *   portMUX_TYPE                 —— 自旋锁对象，portMUX_INITIALIZER_UNLOCKED 表示初始未上锁；
 *   portENTER_CRITICAL(&s_lock)  —— 进临界区：上锁并关中断，保证极短的共享数据访问不被打断；
 *   portEXIT_CRITICAL(&s_lock)   —— 出临界区：恢复中断并解锁。
 * 临界区内只做赋值/比较这类几条指令的操作，绝不能打印日志或做耗时动作。
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 把配置重置为出厂默认值
 *
 * 出厂默认值来自 Kconfig，也就是"编译时确定的初始设定"。
 *
 * @param cfg 输出配置结构体，所有字段都会被写入默认值
 *
 * 注意：必须先于 load_from_nvs() 调用——NVS 里缺失的键就保留这里的默认值，
 * 相当于"先铺一层底，再用 Flash 里存的值逐项覆盖"。
 */
static void load_defaults(settings_t *cfg)
{
    cfg->buzzer_volume = CONFIG_BUZZER_VOLUME_PERCENT;
    cfg->lcd_brightness = 80; /* 80%：室内外通用的出厂观感 */
    cfg->display_rate_hz = 5; /* 5Hz：40Hz 快拍源上取 8 帧窗口平均，稳与快的折中 */
    cfg->orientation_auto = 0;   /* 出厂手动方向（横屏 0°），避免开机乱转 */
    cfg->orientation_angle = 0;  /* 默认横屏 */
    cfg->wave_smooth = 0; /* 波形曲线平滑默认关（忠实反映原始趋势） */
    cfg->wave_window = 1;    /* 波形默认 10s 窗（B20） */
    cfg->wave_mode = 3;      /* 波形默认电压+电流模式（PM_WAVE_MODE_VI=3，与旧版 UI 一致） */
    cfg->screen_off_s = 0;   /* 默认永不自动降亮（F4；0=永不 1=15s 2=30s 3=60s） */
}

/**
 * @brief 把告警阈值重置为出厂默认（全部关闭）
 */
static void alarm_defaults(settings_alarm_t *a)
{
    a->v_en = 0;
    a->i_en = 0;
    a->p_en = 0;
    a->v_limit = SETTINGS_ALARM_V_LIMIT_DEF;
    a->i_limit = SETTINGS_ALARM_I_LIMIT_DEF;
    a->p_limit = SETTINGS_ALARM_P_LIMIT_DEF;
}

/**
 * @brief 打开 NVS 并处理首次上电 / 版本升级两种特殊情况
 *
 * nvs_flash_init() 在两种情况下会返回错误，且都属于正常现象：
 *   · 分区里还没有有效数据（首次上电、刚换芯片）
 *   · 分区的 NVS 版本与当前 IDF 不一致（升级了 IDF）
 * 这两种情况的标准处理方式是"擦除分区并用新格式重建"。
 *
 * @return ESP_OK NVS 可用；其他值表示 NVS 完全不可用（调用方应降级为纯默认值运行）
 */
static esp_err_t nvs_init_with_recovery(void)
{
    /* nvs_flash_init()：初始化 NVS Flash 分区。它会扫描分区、装载键值索引；
     * 首次使用（分区全空）或版本不匹配时会返回下面两种错误码 */
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建（%s），执行擦除", esp_err_to_name(err));
        /* nvs_flash_erase()：擦除整个 NVS 分区，让 nvs_flash_init() 能以全新格式重建。
         * 代价是丢掉所有已保存的配置——对"首次上电/版本升级"场景可接受 */
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "擦除 NVS 失败: %s", esp_err_to_name(err));
            return err;
        }
        /* 擦除后必须再 init 一次，才会真正装载起"空但可用"的 NVS */
        err = nvs_flash_init();
    }

    return err;
}

/**
 * @brief 从 NVS 载入各配置项到缓存（缺失项保持默认值）
 *
 * 载入前 s_cache 必须已填好默认值（见 load_defaults），这里只把
 * "NVS 里确实存在且合法"的项覆盖上去，读失败/值越界都保持默认。
 */
static void load_from_nvs(void)
{
    nvs_handle_t handle;
    /* nvs_open(命名空间, 模式, &句柄)：打开 NVS 里的一个"配置文件夹"。
     * 命名空间相当于文件夹名，键名相当于里面的文件名；NVS_READONLY 表示只读打开 */
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* 首次上电时这个命名空间还不存在，属于正常情况：
         * 缓存里已经是出厂默认值，直接返回即可 */
        ESP_LOGI(TAG, "尚无已保存的配置，使用出厂默认值");
        return;
    }

    /* ---- 蜂鸣器响度 ----
     * nvs_get_u8(句柄, 键名, &输出)：按键名读出一个 uint8_t 值。
     * 键不存在时返回 ESP_ERR_NVS_NOT_FOUND（首次使用时的正常情况） */
    uint8_t volume = 0;
    err = nvs_get_u8(handle, SETTINGS_KEY_BUZZER_VOLUME, &volume);
    if (err == ESP_OK && volume >= SETTINGS_VOLUME_MIN && volume <= SETTINGS_VOLUME_MAX) {
        s_cache.buzzer_volume = volume;
        ESP_LOGI(TAG, "已载入蜂鸣器响度: %u%%", volume);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        /* 值越界说明存储被异常改写，此时保留出厂默认值更安全 */
        ESP_LOGW(TAG, "蜂鸣器响度读取异常（%s，值 %u），使用默认值", esp_err_to_name(err), volume);
    }

    /* ---- 屏幕亮度 ---- */
    uint8_t bright = 0;
    err = nvs_get_u8(handle, SETTINGS_KEY_LCD_BRIGHTNESS, &bright);
    if (err == ESP_OK && bright >= SETTINGS_BRIGHT_MIN && bright <= SETTINGS_BRIGHT_MAX) {
        s_cache.lcd_brightness = bright;
        ESP_LOGI(TAG, "已载入屏幕亮度: %u%%", bright);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "屏幕亮度读取异常（%s，值 %u），使用默认值", esp_err_to_name(err), bright);
    }

    /* ---- 首页数据刷新率 ---- */
    uint8_t rate = 0;
    err = nvs_get_u8(handle, SETTINGS_KEY_DISPLAY_RATE, &rate);
    if (err == ESP_OK && settings_rate_valid(rate)) {
        s_cache.display_rate_hz = rate;
        ESP_LOGI(TAG, "已载入数据刷新率: %u Hz", rate);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "数据刷新率读取异常（%s，值 %u），使用默认值", esp_err_to_name(err), rate);
    }

    /* ---- 屏幕方向（自动开关 + 手动角度）----
     * 角度含 270，超出 uint8_t 范围，必须用 u16 存储（早期版本误用 u8，
     * 270 会被截断成 14，读到类型不匹配的旧键时按 ESP_NVS_NOT_FOUND 处理）。 */
    uint8_t oauto = 0;
    if (nvs_get_u8(handle, SETTINGS_KEY_ORIENT_AUTO, &oauto) == ESP_OK) {
        s_cache.orientation_auto = oauto ? 1u : 0u;
    }
    uint16_t oang = 0;
    if (nvs_get_u16(handle, SETTINGS_KEY_ORIENT_ANG, &oang) == ESP_OK &&
        (oang == 0 || oang == 90 || oang == 180 || oang == 270)) {
        s_cache.orientation_angle = oang;
    }
    ESP_LOGI(TAG, "已载入屏幕方向：自动=%u，角度=%u°", (unsigned)s_cache.orientation_auto,
             (unsigned)s_cache.orientation_angle);

    /* ---- 波形曲线平滑 ---- */
    uint8_t smooth = 0;
    if (nvs_get_u8(handle, SETTINGS_KEY_WAVE_SMOOTH, &smooth) == ESP_OK) {
        s_cache.wave_smooth = smooth ? 1u : 0u;
    }

    /* ---- 波形时间窗/显示模式 + 自动降亮档位（B20/F4）---- */
    uint8_t ww = 0;
    if (nvs_get_u8(handle, SETTINGS_KEY_WAVE_WIN, &ww) == ESP_OK && ww <= 5u) {
        s_cache.wave_window = ww;
    }
    uint8_t wm = 0;
    if (nvs_get_u8(handle, SETTINGS_KEY_WAVE_MODE, &wm) == ESP_OK && wm <= 4u) {
        s_cache.wave_mode = wm;
    }
    uint8_t so = 0;
    if (nvs_get_u8(handle, SETTINGS_KEY_SCR_OFF, &so) == ESP_OK && so <= 3u) {
        s_cache.screen_off_s = so;
    }

    /* ---- 告警阈值（blob）----
     * nvs_get_blob：按键读出一整块二进制数据；长度不符说明是旧布局，
     * 保持出厂默认（全部关闭）更安全。 */
    settings_alarm_t alarm;
    size_t alen = sizeof(alarm);
    if (nvs_get_blob(handle, SETTINGS_KEY_ALARM, &alarm, &alen) == ESP_OK &&
        alen == sizeof(alarm)) {
        s_alarm_cache = alarm;
        ESP_LOGI(TAG, "已载入告警阈值（V:%u %.2fV I:%u %.2fA P:%u %.2fW）",
                 (unsigned)alarm.v_en, (double)alarm.v_limit, (unsigned)alarm.i_en,
                 (double)alarm.i_limit, (unsigned)alarm.p_en, (double)alarm.p_limit);
    }

    /* nvs_close(句柄)：关闭命名空间句柄，释放 NVS 内部的缓存页占用。
     * 句柄用完必须关闭，否则会泄漏 NVS 句柄槽位 */
    nvs_close(handle);
}

/**
 * @brief 把内存缓存写入 NVS（内部使用，调用前需确保已初始化）
 *
 * 整个写入流程是"逐项 set → 最后 commit"：
 * nvs_set_xxx() 只把值写进 RAM 中的 NVS 页缓存，并没有真正写 Flash；
 * 只有 nvs_commit() 才把整批改动一次性落到 Flash。
 * 因此多个键的改动要么一起落盘，要么都不落盘，不会出现写一半的中间状态。
 *
 * 并发保护：整段持 s_save_mutex 串行化（settings_save_now 与防抖落盘任务
 * 都可能进入），避免两路 nvs_set_xxx/commit 交错互相覆盖。
 *
 * @return ESP_OK 全部写入并提交成功；其他值表示失败（此时缓存仍有效，不影响当前运行）
 */
static esp_err_t persist_cache(void)
{
    nvs_handle_t handle;

    xSemaphoreTake(s_save_mutex, portMAX_DELAY);
    /* 与读取不同，这里要用 NVS_READWRITE 打开，否则 nvs_set_xxx 会拒绝写入 */
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败: %s", esp_err_to_name(err));
        xSemaphoreGive(s_save_mutex);
        return err;
    }

    /* nvs_set_u8/nvs_set_u16(句柄, 键名, 值)：按键名写入一个整数值。
     * 任一步失败就不再继续，把错误留到函数末尾统一返回 */
    err = nvs_set_u8(handle, SETTINGS_KEY_BUZZER_VOLUME, s_cache.buzzer_volume);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SETTINGS_KEY_LCD_BRIGHTNESS, s_cache.lcd_brightness);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SETTINGS_KEY_DISPLAY_RATE, s_cache.display_rate_hz);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SETTINGS_KEY_ORIENT_AUTO, s_cache.orientation_auto);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, SETTINGS_KEY_ORIENT_ANG, s_cache.orientation_angle);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SETTINGS_KEY_WAVE_SMOOTH, s_cache.wave_smooth);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SETTINGS_KEY_WAVE_WIN, s_cache.wave_window);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SETTINGS_KEY_WAVE_MODE, s_cache.wave_mode);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, SETTINGS_KEY_SCR_OFF, s_cache.screen_off_s);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, SETTINGS_KEY_ALARM, &s_alarm_cache, sizeof(s_alarm_cache));
    }
    if (err == ESP_OK) {
        /* nvs_set_xxx 只是写入 RAM 中的 NVS 页缓存，
         * 必须 commit 才会真正落到 Flash */
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    xSemaphoreGive(s_save_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 NVS 失败: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 防抖落盘任务：接收定时器通知，执行真正的 NVS 落盘
 *
 * 【并发修复 P2】落盘过去在 esp_timer 回调里做，会阻塞共用 esp_timer
 * 任务的 LVGL tick（界面卡顿）。现在回调只置位通知，写 Flash 在这里做。
 * 两种唤醒方式（见 xEventGroupWaitBits 的超时参数）：
 *   · 防抖定时器到点 → 事件位被置 → 立刻醒来落盘；
 *   · 一直没有通知 → 每 SETTINGS_SAVE_POLL_MS 醒一次，
 *     检查"落盘失败残留的脏标记"并重试（失败不丢数据）。
 */
static void save_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* xEventGroupWaitBits(事件组, 关注位, 清除位, 等任意位?, 超时)：
         * 阻塞等 SETTINGS_EVT_DO_SAVE 置位（pdTRUE = 等到后自动清掉该位）；
         * 等不到就超时返回，做一轮兜底检查。等待期间不占 CPU。 */
        (void)xEventGroupWaitBits(s_save_events, SETTINGS_EVT_DO_SAVE, pdTRUE, pdFALSE,
                                  pdMS_TO_TICKS(SETTINGS_SAVE_POLL_MS));

        /* 进临界区看脏标记：与 mark_dirty_and_schedule() / settings_save_now() 竞争 */
        portENTER_CRITICAL(&s_lock);
        bool need_save = s_dirty;
        portEXIT_CRITICAL(&s_lock);

        /* 静默期内可能已被 settings_save_now() 提前落盘并清掉脏标记，
         * 这里就无事可做——不重复写 Flash，也不多耗一次擦写寿命 */
        if (!need_save) {
            continue;
        }

        if (persist_cache() == ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_dirty = false;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "配置已保存（防抖 %d ms 后落盘）", SETTINGS_SAVE_DELAY_MS);
        }
        /* persist_cache() 失败时故意保留脏标记：下一轮周期检查
         * （或下一次改动/ settings_save_now()）还会再次尝试落盘 */
    }
}

/**
 * @brief 防抖定时器回调：静默期结束，通知落盘任务执行落盘
 *
 * 【并发修复 P2】esp_timer 的回调运行在共用的 esp_timer 任务上下文里
 * （与 LVGL 的心跳 tick 同任务）。因此这里**绝不做 NVS 写入**
 * （nvs_commit 可达几十~几百 ms，会拖住 LVGL tick 造成界面卡顿），
 * 只做"通知"这一条轻量动作：置事件位唤醒 save_task，回调随即返回。
 *
 * @param arg 创建定时器时传入的用户参数（本模块未使用）
 */
static void save_timer_callback(void *arg)
{
    (void)arg;

    /* xEventGroupSetBits：置位并唤醒正在 xEventGroupWaitBits 等待的
     * save_task。该函数在任务/回调上下文里均可调用（不是 ISR 专用版） */
    xEventGroupSetBits(s_save_events, SETTINGS_EVT_DO_SAVE);
}

/**
 * @brief 标记"有改动待保存"，并重置防抖定时器
 *
 * 每次设置变化都会调用它。定时器不断被重置，只有连续静默
 * SETTINGS_SAVE_DELAY_MS 毫秒后才会真正写 Flash——这就是"防抖"。
 */
static void mark_dirty_and_schedule(void)
{
    /* 脏标记被任务与定时器回调共享，进临界区改 */
    portENTER_CRITICAL(&s_lock);
    s_dirty = true;
    portEXIT_CRITICAL(&s_lock);

    /* 先停止再启动：esp_timer 要求启动前定时器处于停止状态。
     * 每来一次新改动就重置，从而实现"静默一段时间后才落盘"。
     * esp_timer_stop()：停止定时器（没在运行时返回错误，这里可以忽略）。
     * esp_timer_start_once()：启动一个"只触发一次"的定时器，参数单位是微秒
     * （所以毫秒值要乘 1000ULL）；回调即上面的 save_timer_callback() */
    esp_timer_stop(s_save_timer);
    esp_err_t err = esp_timer_start_once(s_save_timer, (uint64_t)SETTINGS_SAVE_DELAY_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "启动防抖定时器失败: %s", esp_err_to_name(err));
    }
}

/**
 * @brief 初始化配置服务：初始化 NVS 并把配置载入内存缓存
 *
 * 完整的初始化顺序与容错策略见函数体内的 ①②③④ 注释。
 *
 * @return ESP_OK 成功（含"NVS 不可用但已用默认值"的降级情况，
 *         此时会额外上报 SYSTEM_FAULT_STORAGE 让故障可见）
 *         ESP_ERR_NO_MEM 创建定时器/同步原语/落盘任务失败
 */
esp_err_t settings_init(void)
{
    /* 防重复初始化：重复创建定时器会泄漏句柄 */
    if (s_ready) {
        ESP_LOGW(TAG, "已初始化，忽略重复调用");
        return ESP_OK;
    }

    /* ① 先把缓存填上出厂默认值：这样即使后面 NVS 出问题，
     *    设备也一定能拿到一份合法配置继续运行（优雅降级） */
    load_defaults(&s_cache);
    alarm_defaults(&s_alarm_cache);

    /* ② 创建防抖定时器（与 NVS 是否可用无关，先建好备用）。
     *    esp_timer_create_args_t 是定时器参数结构体：
     *      callback —— 到点时调用的函数；arg —— 传给回调的参数；name —— 任务名前缀（便于调试）。
     *    esp_timer_create() 按参数创建定时器，返回句柄供后续 start/stop 使用 */
    const esp_timer_create_args_t timer_cfg = {
        .callback = save_timer_callback,
        .arg = NULL,
        .name = "cfg_save",
    };
    esp_err_t err = esp_timer_create(&timer_cfg, &s_save_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建防抖定时器失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ③ 创建落盘任务的同步原语与任务本身（落盘任务化，见文件头说明）。
     *    xEventGroupCreate：创建事件组；xSemaphoreCreateMutex：创建互斥锁；
     *    xTaskCreatePinnedToCore(入口, 名称, 栈字节, 参数, 优先级, 句柄出参, 核心号)。
     *    绑 Core0（交互核）：落盘不紧急，别去测量核（Core1）抢采样时间片。
     *    任一步失败直接返回错误码（此时 s_ready 尚未置位，可修复后重试） */
    s_save_events = xEventGroupCreate();
    s_save_mutex = xSemaphoreCreateMutex();
    if (s_save_events == NULL || s_save_mutex == NULL) {
        ESP_LOGE(TAG, "创建落盘同步原语失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(save_task, "cfg_save_task", SETTINGS_SAVE_TASK_STACK,
                                            NULL, SETTINGS_SAVE_TASK_PRIO, &s_save_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建防抖落盘任务失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;

    /* ④ 尝试初始化 NVS 并载入已保存的配置。
     *    失败不回滚 s_ready —— 允许"只能读默认值、无法保存"的降级运行。
     *    【修复 P2】但要把故障上报出去：存储坏了若无声无息，
     *    排查时会以为配置真的"没保存住"。上报 SYSTEM_FAULT_STORAGE
     *    （非致命位）后状态机进入 DEGRADED、指示灯可见。
     *    仍返回 ESP_OK 保证设备可继续用（测量/显示都不依赖 NVS）。 */
    err = nvs_init_with_recovery();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s，本次运行将使用出厂默认值（设置无法保存）",
                 esp_err_to_name(err));
        (void)system_state_report_fault(SYSTEM_FAULT_STORAGE);
        return ESP_OK;
    }

    load_from_nvs();

    ESP_LOGI(TAG, "初始化完成（响度 %u%%，亮度 %u%%）", s_cache.buzzer_volume,
             s_cache.lcd_brightness);
    return ESP_OK;
}

/**
 * @brief 读取当前生效的配置（从内存缓存，速度很快）
 *
 * @param out 输出配置，不可为 NULL
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG out 为 NULL
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_get(settings_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 结构体很小（目前约 10 字节），用临界区拷贝即可保证一致性，
     * 不必引入互斥锁 */
    portENTER_CRITICAL(&s_lock);
    *out = s_cache;
    portEXIT_CRITICAL(&s_lock);

    return ESP_OK;
}

/**
 * @brief 更新蜂鸣器响度：写入内存缓存并安排延迟落盘
 *
 * @param percent 响度 1~100
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 超出范围
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_set_buzzer_volume(uint8_t percent)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < SETTINGS_VOLUME_MIN || percent > SETTINGS_VOLUME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    s_cache.buzzer_volume = percent;
    portEXIT_CRITICAL(&s_lock);

    /* 只更新缓存 + 安排延迟落盘，不立即写 Flash（防磨损） */
    mark_dirty_and_schedule();

    return ESP_OK;
}

/**
 * @brief 更新屏幕背光亮度：写入内存缓存并安排延迟落盘
 *
 * 亮度"立即生效"由调用方负责同步调用背光驱动，本函数只管保存。
 *
 * @param percent 亮度 10~100（下限 10 防止调到全黑后无法操作）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 超出范围
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_set_lcd_brightness(uint8_t percent)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < SETTINGS_BRIGHT_MIN || percent > SETTINGS_BRIGHT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    s_cache.lcd_brightness = percent;
    portEXIT_CRITICAL(&s_lock);

    mark_dirty_and_schedule();

    return ESP_OK;
}

/**
 * @brief 更新首页数据刷新率（测量页快速菜单中可调）
 *
 * @param hz 刷新率，仅接受 1/5/10/20/30（见 settings_rate_valid）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 非合法档位
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_set_display_rate_hz(uint8_t hz)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!settings_rate_valid(hz)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    s_cache.display_rate_hz = hz;
    portEXIT_CRITICAL(&s_lock);

    mark_dirty_and_schedule();

    return ESP_OK;
}

/**
 * @brief 更新屏幕方向设置（显示设置页调用；立即防抖落盘）
 *
 * @param auto_on 是否启用重力自动旋转（1/0）
 * @param angle   手动方向角度（auto_on=0 时生效；仅接受 0/90/180/270）
 * @return ESP_OK 成功
 *         ESP_ERR_INVALID_ARG 非法角度
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t settings_set_orientation(uint8_t auto_on, uint16_t angle)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (angle != 0 && angle != 90 && angle != 180 && angle != 270) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 开关与角度放在同一个临界区里更新，避免读者看到
     * "新开关 + 旧角度"的不一致组合 */
    portENTER_CRITICAL(&s_lock);
    s_cache.orientation_auto = auto_on ? 1u : 0u;
    s_cache.orientation_angle = angle;
    portEXIT_CRITICAL(&s_lock);

    mark_dirty_and_schedule();

    return ESP_OK;
}

/**
 * @brief 设置波形曲线平滑开关
 *
 * @param on 1=开启平滑，0=关闭（默认）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t settings_set_wave_smooth(uint8_t on)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    s_cache.wave_smooth = on ? 1u : 0u;
    portEXIT_CRITICAL(&s_lock);

    mark_dirty_and_schedule();

    return ESP_OK;
}

/**
 * @brief 立即把内存缓存写入 NVS（忽略防抖）
 *
 * 典型用法：用户离开设置页、或设备即将关机前调用。
 *
 * @return ESP_OK 成功（含"无待保存改动"的空操作情况）
 *         ESP_ERR_INVALID_STATE 尚未初始化
 *         其他值 NVS 写入失败（内存缓存仍然有效，不影响当前运行）
 */
esp_err_t settings_save_now(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 没有待落盘的改动就直接成功返回，不碰 Flash。
     * 开机时 device_ctrl 会用“从 NVS 恢复出来的值”调一次本函数，那些值本就
     * 已在 NVS 中，无需再写；只有运行期用户真的改过（s_dirty=true）才需要落盘，
     * 从而避免每次开机都做一次无谓的 NVS commit（减少 Flash 磨损）。 */
    portENTER_CRITICAL(&s_lock);
    bool need_save = s_dirty;
    portEXIT_CRITICAL(&s_lock);
    if (!need_save) {
        return ESP_OK;
    }

    /* 取消待执行的防抖任务，改为当前立即落盘。
     * esp_timer_stop()：停掉还没到点的单次定时器（没在运行也无害） */
    esp_timer_stop(s_save_timer);

    esp_err_t err = persist_cache();
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_dirty = false;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "配置已立即保存");
    }
    return err;
}

esp_err_t settings_request_save(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ① 置脏标记（与 save_task / 防抖定时器回调共享，进临界区改） */
    portENTER_CRITICAL(&s_lock);
    s_dirty = true;
    portEXIT_CRITICAL(&s_lock);

    /* ② 取消可能待触发的防抖定时器：同一批改动不必被通知两次
     * （esp_timer_stop：定时器没在运行会返回错误，这里无害地忽略） */
    esp_timer_stop(s_save_timer);

    /* ③ 直接置事件位唤醒 save_task。本函数因此是**非阻塞**的：
     * 调用者（可能是持有 LVGL 锁的 UI 事件回调）几微秒内即可返回，
     * 真正的 nvs_set_xxx/commit 全部在 save_task 里完成。 */
    xEventGroupSetBits(s_save_events, SETTINGS_EVT_DO_SAVE);
    return ESP_OK;
}

esp_err_t settings_get_alarm(settings_alarm_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 缓存读取与 set 竞争，短临界区拷一份（约 16 字节，几条指令） */
    portENTER_CRITICAL(&s_lock);
    *out = s_alarm_cache;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t settings_set_alarm(const settings_alarm_t *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 校验：阈值必须是正数且不超过整机量程上限（拒绝 NaN/inf/≤0/越界），
     * 非法输入整体拒绝保持原值。
     * NaN 与 +inf 的判据：NaN 的任何比较均为 false，`!(x > 0)` 会命中；
     * +inf 虽 >0，但 `x > MAX` 会命中——两条合起来即可过滤非有限值。 */
    if (!(in->v_limit > 0.0f) || (in->v_limit > SETTINGS_ALARM_V_MAX) ||
        !(in->i_limit > 0.0f) || (in->i_limit > SETTINGS_ALARM_I_MAX) ||
        !(in->p_limit > 0.0f) || (in->p_limit > SETTINGS_ALARM_P_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_alarm_t norm = *in;
    norm.v_en = norm.v_en ? 1u : 0u;
    norm.i_en = norm.i_en ? 1u : 0u;
    norm.p_en = norm.p_en ? 1u : 0u;

    portENTER_CRITICAL(&s_lock);
    s_alarm_cache = norm;
    portEXIT_CRITICAL(&s_lock);

    /* 与其他设置一致走防抖落盘（频繁拖动阈值滑条不烧 Flash） */
    mark_dirty_and_schedule();
    return ESP_OK;
}

esp_err_t settings_set_wave_window(uint8_t window)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (window > 5u) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    s_cache.wave_window = window;
    portEXIT_CRITICAL(&s_lock);
    mark_dirty_and_schedule();
    return ESP_OK;
}

esp_err_t settings_set_wave_mode(uint8_t mode)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode > 4u) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    s_cache.wave_mode = mode;
    portEXIT_CRITICAL(&s_lock);
    mark_dirty_and_schedule();
    return ESP_OK;
}

esp_err_t settings_set_screen_off(uint8_t level)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (level > 3u) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    s_cache.screen_off_s = level;
    portEXIT_CRITICAL(&s_lock);
    mark_dirty_and_schedule();
    return ESP_OK;
}

esp_err_t settings_restore_factory(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 擦除 xt60cfg 整命名空间（含告警阈值 blob 与全部用户偏好）；
     * 标定在 pm 命名空间由 power_measure 自管，此处不触碰（保留） */
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    /* 无论 NVS 操作成败，内存缓存都回出厂默认（立即生效） */
    portENTER_CRITICAL(&s_lock);
    load_defaults(&s_cache);
    alarm_defaults(&s_alarm_cache);
    portEXIT_CRITICAL(&s_lock);
    s_dirty = false; /* 擦除后无需再落默认值（下次改动自然写入） */
    esp_timer_stop(s_save_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "恢复出厂：NVS 擦除失败（%s），仅内存已回默认", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "恢复出厂完成：用户设置已清空并回到出厂默认");
    }
    return err;
}
