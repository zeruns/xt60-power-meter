# 固件架构说明（XT60 功率计）

面向需要接手 / 学习本工程的开发者，重点解释**为什么这样设计**，而不只是罗列文件。

---

## 1. 分层结构

```
┌─────────────────────────────────────────────────────┐
│ main/            应用入口与启动编排（组合根）          │
│                  main.c / app_init.c / app_tasks.c   │
├─────────────────────────────────────────────────────┤
│ services/        业务服务层                          │
│                  system_state  power_measure         │
│                  device_ctrl   settings   ui         │
├─────────────────────────────────────────────────────┤
│ middleware/      中间件 / 适配层                     │
│                  event_bus  data_store               │
│                  lvgl_port  wifi_manager  web_console│
│                  lvgl、esp_lvgl_port（组件管理器托管） │
├─────────────────────────────────────────────────────┤
│ drivers/         设备驱动层（每个外设一个组件）        │
│                  status_led  ads131m02  lcd_nv3030b  │
│                  touch_cst816d  imu_sc7a20h  aht20   │
│                  buzzer  battery_adc  button         │
├─────────────────────────────────────────────────────┤
│ bsp/             板级支持层（引脚唯一真相源）          │
├─────────────────────────────────────────────────────┤
│ 外部依赖（全部由组件管理器托管于 managed_components/）│
│   lvgl/lvgl · espressif/esp_lvgl_port                │
│   espressif/esp_lcd_spd2010                          │
├─────────────────────────────────────────────────────┤
│ ESP-IDF v5.5+（开发基线 v6.1）· FreeRTOS · ESP32-S3   │
└─────────────────────────────────────────────────────┘
```

**依赖方向只能自上而下**（上层依赖下层，下层不认识上层）：

| 层 | 允许依赖 | 禁止 |
|---|---|---|
| `main` | 全部（它是组合根，负责把各层接起来） | — |
| `services` | `middleware`、`bsp` | 直接调用 ESP-IDF 外设 API |
| `middleware` | `drivers`、`bsp` | 依赖 `services` |
| `drivers` | `bsp`、ESP-IDF 驱动组件 | 依赖 `middleware`、`services` |
| `bsp` | ESP-IDF 基础组件 | 其余全部 |

两条硬性约束：

1. **业务代码里不得出现 `gpio_set_level` / `spi_device_*` / `i2c_master_*` 等裸外设调用**，必须经 `drivers` 封装；
2. **引脚编号只允许出现在 `components/bsp/include/board_pins.h`**，其他文件一律引用 `BOARD_PIN_*` 宏。

### 外部依赖：统一由组件管理器托管

工程里的第三方代码**全部由 ESP 组件管理器托管**（落在 `managed_components/`，由各组件目录下的 `idf_component.yml` 声明）：

| 组件 | 声明位置 | 版本 |
|---|---|---|
| `lvgl/lvgl` | `components/middleware/lvgl_port/idf_component.yml` | 9.6.0~1 |
| `espressif/esp_lvgl_port` | 同上 | 2.9.0 |
| `espressif/esp_lcd_spd2010` | `components/drivers/lcd_nv3030b/idf_component.yml` | 1.0.2（及其依赖 `espressif/cmake_utilities`） |

规则：
- **不再 vendor 任何第三方源码**（早期 LVGL 曾 vendor 到 `components/middleware/lvgl/`，现已移除）。
- 托管组件的版本记录在 `dependencies.lock`（入库），保证换机器能复现同一版本；升级时改
  `idf_component.yml` 的版本号后 `idf.py reconfigure` 即可。
- 托管组件的 CMake 目标名为「`<命名空间>__<组件名>`」（如 `lvgl__lvgl`、`espressif__esp_lvgl_port`），
  `REQUIRES` 里必须用这个全名。
- ⚠ **组件重名规避**：官方 `espressif/esp_lvgl_port` 的 CMake 会探测名为 `button` 的组件并编译其
  iot_button 适配（`esp_lvgl_port_button.c`）。因此本工程的按键驱动组件命名为 `key_button`，
  **不能**叫 `button`，否则会被按 iot_button 的 API 编译而报错。

---

## 2. 状态机与指示灯：依赖倒置（方案 B）

早期版本里，状态机直接调用 `status_led_set_pattern()` 去改灯 —— 这违反了分层规则（业务层依赖驱动层）。现已改为事件驱动：

```
        ┌──────────────────┐  广播 APP_EVENT_SYSTEM_STATE_CHANGED
        │  system_state    │──────────────────────┐
        │  （状态机）       │                      │
        └──────────────────┘                      ▼
                                          ┌───────────────┐
                                          │  event_bus    │
                                          └───────┬───────┘
                                                  │ 分发
                                                  ▼
                                          ┌───────────────┐
                                          │  device_ctrl  │──► status_led
                                          │ （订阅者）     │    （驱动）
                                          └───────────────┘
```

关键点：**状态机完全不知道谁在听**。要新增"故障时蜂鸣器长鸣"，只需再写一个订阅者，状态机代码一行都不用改。

### 状态模型

状态不是随意设置的，而是由**故障位图**自动推导：

| 条件 | 状态 | 指示灯 | 蜂鸣器 |
|---|---|---|---|
| 启动未完成 | `INIT` | 常亮 | 静音 |
| 无故障位 | `RUNNING` | 慢闪 1 Hz | 静音 |
| 有非致命故障位 | `DEGRADED` | 双闪 + 长灭 | 静音 |
| 命中致命掩码 | `FAULT` | 快闪 5 Hz | **每 1 秒长鸣一声** |

```
SYSTEM_FAULT_CRITICAL_MASK = SYSTEM_FAULT_ADC | SYSTEM_FAULT_INFRA
```

判断依据：「这个模块没了，设备还算不算一台能用的测量仪器」。ADC 坏了功率计就没意义；屏幕、WiFi、传感器坏了只是少个功能，属降级。

模块只需 `system_state_report_fault(SYSTEM_FAULT_LCD)` 上报症状，不必关心最终该是什么状态。

**降级（DEGRADED）为什么不报警**：为避免用户因一个非关键外设故障被持续打扰，降级只用指示灯（双闪）区分，只有真正不可用的 FAULT 才发声。

**启动完成提示音**：启动流程走完（无致命故障）后，由 `app_init_startup()` 调用 `device_ctrl_notify_boot_ok()` 短鸣一声。
这个声音刻意不放在 `buzzer_init()` 里 —— 它表达的是"整个系统已就绪"这一系统级含义，属于业务层职责；放进驱动初始化会产生难以预期的副作用（例如将来单独测试蜂鸣器时会莫名响一声）。

---

## 3. 启动流程与分级容错

### 为什么不用 `ESP_ERROR_CHECK`

`ESP_ERROR_CHECK` 失败会 `abort()` → 复位 → 再失败 → 再复位，形成**无限重启**。对装到用户手里的仪器，这是最糟的故障模式：屏幕黑、灯不亮、无法通过 OTA 救回。

### 初始化表

`main/app_init.c` 用一张表描述启动步骤，每项登记自己的故障位（下表为真实内容，共 17 项）：

```c
static const init_step_t s_boot_steps[] = {
    /* 软件基础设施 */
    { "event_bus",     event_bus_init,     SYSTEM_FAULT_INFRA   },  /* 关键 */
    { "status_led",    status_led_init,    SYSTEM_FAULT_LED     },
    { "buzzer",        buzzer_init,        SYSTEM_FAULT_BUZZER  },
    { "system_state",  system_state_init,  SYSTEM_FAULT_INFRA   },  /* 关键 */
    { "settings",      settings_init,      SYSTEM_FAULT_STORAGE },
    { "device_ctrl",   device_ctrl_init,   SYSTEM_FAULT_LED     },
    /* 数据通道 */
    { "data_store",    data_store_init,    SYSTEM_FAULT_INFRA   },  /* 关键 */
    /* 传感器与测量前端 */
    { "button",        button_init,        SYSTEM_FAULT_BUTTON  },
    { "battery_adc",   battery_adc_init,   SYSTEM_FAULT_BATTERY },
    { "aht20",         aht20_init,         SYSTEM_FAULT_ENV     },
    { "imu_sc7a20h",   imu_sc7a20h_init,   SYSTEM_FAULT_IMU     },
    { "ads131m02",     ads131m02_init,     SYSTEM_FAULT_ADC     },  /* 关键，4kSPS */
    { "power_measure", power_measure_init, SYSTEM_FAULT_ADC     },  /* 测量服务：NVS/波形缓冲 */
    /* 显示与交互 */
    { "fs_storage",    fs_storage_init,    SYSTEM_FAULT_STORAGE },  /* /spiffs 挂载，字体在此 */
    { "lcd_nv3030b",   lcd_nv3030b_init,   SYSTEM_FAULT_LCD     },
    { "touch_cst816d", touch_cst816d_init, SYSTEM_FAULT_TOUCH   },
    { "lvgl_port",     lvgl_port_init,     SYSTEM_FAULT_LCD     },
};
```

容错规则：

- **关键步骤失败**（故障位命中 `CRITICAL_MASK`，即 `SYSTEM_FAULT_ADC` 与 `SYSTEM_FAULT_INFRA`）→ 跳过后续关键步骤，但**继续执行非关键步骤**（尽量保住屏幕 / WiFi 这些"救回通道"），最终进入 `FAULT` 状态且**不复位**；
- **非关键步骤失败** → 只登记故障位，系统降级继续运行。

顺序约束（四条，改表时必须保持）：

1. `event_bus` 最先（其他模块靠它通信）；
2. `status_led` 早于 `system_state` / `device_ctrl`（状态机初始化会广播状态，`device_ctrl` 收到后要立刻设置指示灯）；
3. `buzzer` / `settings` 早于 `device_ctrl`（它初始化时会按当前状态同步输出并读取已保存的用户偏好）；
4. 传感器与测量前端早于显示与交互（先把"数据怎么来"准备好，再启动"数据怎么显示"）。

### 启动自检（阶段二）

初始化的"成功"只代表寄存器配置写下去了，**不代表器件真能出数据**。典型反例：
I2C 器件地址对但时序不对（能应答、读不出有效数据）；SPI ADC 的输入时钟缺失
（配置全成功，但 DRDY# 永远不来）。

因此在初始化表跑完之后、进入运行态之前，`app_init.c` 会再跑一轮**硬件自检**：
对每个外设**真读一次数据**。

| 自检项 | 探测方式 | 故障位 | 关键 |
|---|---|---|---|
| `ads131m02` | `ads131m02_read()` 能读到一帧 | `SYSTEM_FAULT_ADC` | **是** |
| `aht20` | `aht20_read()` 且读数落在合理量程内 | `SYSTEM_FAULT_ENV` | 否 |
| `imu_sc7a20h` | `imu_sc7a20h_get_orientation()` | `SYSTEM_FAULT_IMU` | 否 |
| `battery_adc` | `battery_adc_read()` | `SYSTEM_FAULT_BATTERY` | 否 |
| `touch_cst816d` | `touch_cst816d_read()`（此时未按下，未按下也算通过） | `SYSTEM_FAULT_TOUCH` | 否 |

按键不在自检表内：驱动只提供事件回调，没有可供查询的读取接口。
初始化阶段已失败的模块会被自检跳过（不再重复报同一个故障）。

判定与听觉反馈：

- **关键项失败** → 登记故障位、**不进入运行态**（状态保持 `FAULT`）、**不短鸣**；
- **非关键项失败** → 登记故障位、以 `DEGRADED` 进入运行态、**不短鸣**；
- **全部通过** → 进入运行态并**短鸣一声**。

短鸣的语义被刻意定义为"全部就绪"：降级时鸣一声会误导用户。降级项由自检日志与指示灯双闪体现。

---

## 4. 双核分工

ESP32-S3 双核。ESP-IDF 默认把 WiFi 任务钉在 Core 0，因此按"通信与交互" vs "测量"划分最省事也最合理：

| 核 | 职责 | 任务 | 优先级 |
|---|---|---|---|
| **Core 0**（PRO_CPU） | 交互与通信 | `task_ui`(12)、`lvgl_port`(10)、`event_bus`(6)、`pm_aux`(5)、`orientation`(4)、`sys_monitor`(3)、`cfg_save_task`(2) | 中低 |
| **Core 1**（APP_CPU） | 测量（独占） | `pm_sample`(22)：采样 + 200ms 块计算 | 高 |

优先级遵循**速率单调**原则：周期越短、实时性要求越高，优先级越高。观察/维护类任务（`sys_monitor`、`cfg_save_task`）刻意压低优先级，绝不能影响业务任务。

**为什么不把测量放 Core 0**：WiFi 协议栈会周期性占用较多 CPU，与 4 kHz 采样抢核会造成采样抖动，直接影响测量精度。用硬件资源换时序确定性，是仪器类固件的常规做法。

### 跨核通信规则

只允许两条通道：

1. **`data_store`** —— 测量核写、交互核读（seqlock 无锁）；
2. **`event_bus`** —— 任何核都可发布 / 订阅。

**禁止跨核直接共享可变全局变量**，那会导致难以复现的时序 bug。

---

## 5. data_store：为什么用 seqlock 而不是互斥锁

测量核以 40 Hz 写入（每 25ms 显示子窗写一次快照）、交互核约 30 Hz 读取。直觉做法是加锁，但有两个问题：

- 测量核不能被阻塞（2 ms 样本周期很紧，等锁会破坏采样时序）；
- 快照是 24 字节的结构体，**多字节读写没有原子性**。

第二点的危害很隐蔽：读者可能在写者刚写完电压、还没写电流时读走整份数据，得到"新电压配旧电流"的错乱组合，算出的功率瞬间跳变 —— UI 上表现为曲线毛刺，且极难复现。这称为**数据撕裂（data tearing）**。

seqlock 方案：

```
写入前 seq 变奇数（数据不可信）→ 拷贝数据 → seq 变偶数（数据完整）
读取时：读 seq → 拷数据 → 再读 seq，两次相同且为偶 → 数据一致
```

优点：写方永不被阻塞；读方偶尔重读一次（代价几十纳秒）。

**前提：写者只能有一个**。将来若出现第二个写者，必须改回互斥锁方案。

---

## 6. 可配置性与可测试性

### 配置模型：分两层

本工程有两类"可调参数"，来源与生效方式完全不同，**不要混淆**：

| | 编译期固定项 | 运行时可调项 |
|---|---|---|
| 例子 | 蜂鸣器频率/时长、指示灯闪烁时序、任务周期/栈/优先级 | 蜂鸣器响度 |
| 来源 | Kconfig（`CONFIG_*` 宏） | 运行时变量 + NVS |
| 用户能否改 | 否，需重新编译烧录 | 是，UI 上随时可改 |
| 持久化 | 不涉及（固化在固件里） | 存 NVS，开机自动恢复 |
| 读取方式 | 代码直接使用 `CONFIG_*` 宏 | `settings_get()` / `buzzer_get_volume()` |

Kconfig 位置一览：

| 位置 | 内容 |
|---|---|
| `components/drivers/buzzer/Kconfig.projbuild` | 方波频率、提示音时长、报警周期、**响度出厂默认值** |
| `components/drivers/status_led/Kconfig.projbuild` | 四种闪烁模式的时序 |
| `main/Kconfig.projbuild` | 巡检任务周期 / 栈 / 优先级 |

#### 三条必须遵守的规则

**① 驱动代码里不要写"Kconfig 优先、否则用本地默认值"的回退写法。**

```c
/* ✗ 错误示范：Kconfig 已定义该项时，#else 分支是永远不编译的死代码 */
#ifdef CONFIG_BUZZER_FREQ_HZ
#define BUZZER_FREQ_HZ CONFIG_BUZZER_FREQ_HZ
#else
#define BUZZER_FREQ_HZ 4000        /* 改这一行没有任何效果，且不报错 */
#endif
```
这种写法会让"改了源码里的值却没效果"，极难排查。本工程统一**直接使用 `CONFIG_*` 宏，不设本地默认值**——若 Kconfig 未生效会编译报错，问题立刻暴露。

**② 改 Kconfig 的 `default` 不影响已存在的 `sdkconfig`。**
必须走 `idf.py menuconfig`，或改完 default 后删除 `sdkconfig` 重新生成。

**③ 运行时可调项的"出厂默认值"在 Kconfig，"用户当前值"在 NVS。**
Kconfig 里的响度只是初值。若用户已在设备上改过音量，NVS 中的用户值会覆盖它——此时改 Kconfig 并重新烧录**不会**改变设备响度，需先擦除 NVS 分区（`erase_region 0x9000 0x6000`）。

### 运行时配置的持久化（settings 服务）

`components/services/settings/` 负责保存用户偏好，结构是三层：

```
内存缓存  ← 读取只碰这里，可高频调用（UI 每帧读都不怕）
   ↑ 写
  NVS    ← 真正的持久化，掉电不丢
   ↑ 兜底
Kconfig  ← 出厂默认值（首次上电 / NVS 损坏时使用）
```

**防抖落盘**：UI 用滑块调音量时会产生几十次数值变化，每次写 Flash 会显著加速 NVS 磨损（扇区寿命约 10 万次擦写）。因此每次改动只更新缓存并重置定时器，**连续 1.5 秒无新改动才真正写 NVS**——无论怎么拖动，一次调节最多一次写入。需要立刻落盘的场合（用户离开设置页、关机前）调用 `settings_save_now()`。

**优雅降级**：NVS 不可用时会尝试擦除重建；彻底失败也只记录日志并用出厂默认值继续运行，设备仍能正常测量。因此 settings 在启动表中是非关键模块（故障位 `SYSTEM_FAULT_STORAGE`）。

**写 Flash 的副作用**：ESP32 写 Flash 期间 cache 被禁用，所有执行 Flash 中代码的任务会短暂停滞（十几到几十毫秒）。这是硬件特性，无法靠换任务规避；防抖策略恰好把写入集中到"用户停止调节"之后，影响最小。

### 测试金字塔

| 层 | 内容 | 位置 |
|---|---|---|
| 底层（已完成） | 纯算法：24 位补码符号扩展、标定换算、功率、电能积分 | `components/services/power_measure/src/power_calc.c`，测试在 `test/host/` |
| 中层（待补） | 驱动寄存器解析逻辑，用桩件模拟总线 | `test/target/` |
| 顶层 | 整机联调 | 上板 |

`power_calc.c` **不包含任何 ESP-IDF 头文件**，因此可在 PC 上直接编译测试：

```
gcc -std=c11 -Wall -Wextra -I components/services/power_measure/include \
    test/host/test_power_calc.c \
    components/services/power_measure/src/power_calc.c -o test_power_calc.exe
```

或使用 `test/host/run_host_tests.ps1`（自动寻找编译器；无编译器时提示并跳过）。

> 注意：本机未安装主机端编译器（只有 ESP-IDF 的 xtensa 交叉工具链），
> 因此该测试目前只做过语法校验，尚未实际运行。装上 gcc / clang 后即可执行。

---

## 7. 硬件与构建配置基线（N16R8）

| 项 | 值 | 说明 |
|---|---|---|
| 模组 | ESP32-S3-WROOM-1-**N16R8** | N16 = 16MB 四线 Flash；R8 = 8MB **八线** PSRAM（占用 IO33~37） |
| Flash 模式 | **QIO @ 80MHz** | 四线读，带宽约为 DIO 的 2 倍；QIO 只与「八线 Flash」互斥，与八线 PSRAM 不冲突 |
| PSRAM | Octal @ 80MHz，开内存自检 | 小分配先内部、不足溢出 PSRAM（ALWAYSINTERNAL=0）；WiFi/LWIP 缓冲放 PSRAM |
| CPU 主频 | **240 MHz** | S3 最高主频 |
| 缓存 | 指令 32KB + 数据 64KB | S3 推荐配置，影响 LVGL 与 PSRAM 访问速度 |
| 编译优化 | **-O2（PERF）** | 仪器项目性能优先；调试汇编时可临时改回 -Og |
| 系统节拍 | 1000 Hz | 1 tick = 1ms，供显示/逻辑定时（采样由 DRDY 硬件自节奏） |
| 分区表 | 双 4MB OTA + 64KB coredump + 8MB storage | 用满 16MB `0x830000 + 0x7D0000 = 0x1000000` |
| 崩溃取证 | coredump 写 Flash，panic 后延时 3 秒复位 | 野外设备唯一的崩溃定位手段 |
| 控制台 | USB-Serial/JTAG | 烧录与日志共用一根 USB 线 |

**不要开启 `CONFIG_NEWLIB_NANO_FORMAT`** —— 精简版格式化不支持 `%f`，
会导致日志里的浮点测量值打印为空或乱码（当前配置已确认关闭）。

**关于任务栈与 PSRAM**：`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` 保持开启（IDF 默认），
它允许把任务栈放进 PSRAM 以节省内部 RAM。但**中断服务程序运行在当前任务栈上，
且中断期间不能访问 PSRAM**，因此凡可能被中断打断时正在使用的任务
（尤其测量采样任务），其栈必须在内部 RAM —— 创建时不要传 `MALLOC_CAP_SPIRAM`。

**小分配内存策略（v0.6.1 根因修复）**：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`
——普通 `malloc` 先从内部 RAM 分配、不足时自动溢出到 PSRAM，分配不会因阈值失败。
演进史：16KB/8KB/1KB 档曾把 752B WiFi beacon 缓冲强制推入内部 RAM 导致驱动崩溃，
降到 512 后又把 LVGL（clib malloc）高频小分配（建对象 / label 刷新 strdup）与
WS 10Hz 推送缓冲压进仅剩 ~19KB 的内部 RAM，碎片化耗尽后 `lv_malloc` 返回 NULL、
渲染路径解引用崩溃（表现为"设置页概率卡死、卡几秒自动复位"）。显式
`MALLOC_CAP_INTERNAL` 的分配（ISR 相关、DMA）不受该开关影响。

---

## 8. 诊断与崩溃取证

| 手段 | 配置 / 位置 | 作用 |
|---|---|---|
| 分级日志 | `CONFIG_LOG_DEFAULT_LEVEL_INFO` | 每模块独立 TAG，便于过滤 |
| 系统巡检 | `task_sys_monitor` | 周期输出状态、故障、堆水位、栈水位、数据更新计数 |
| 栈水位 | `uxTaskGetStackHighWaterMark` | 把"栈该给多大"从猜测变成实测 |
| 崩溃转储 | `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH` + `coredump` 分区 | 野外设备崩溃后唯一的取证手段 |
| panic 延时 | `CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=3` | 留时间把日志打完（默认 0 秒会来不及） |
| 事件丢弃计数 | `event_bus_dropped_count()` | 判断是否有订阅者处理过慢 |

---

## 9. 嵌入式专用约束（实现驱动时必须遵守）

### 中断服务程序（ISR）

- **ISR 中禁止访问 PSRAM**。ESP32-S3 的 PSRAM 走 SPI，访问时若 cache 未命中会触发异常；ISR 上下文无法处理。因此 ISR 及其调用的函数、数据必须在内部 RAM（加 `IRAM_ATTR`）。
- **ISR 必须简短**。ADS131M02 的 DRDY# 中断里只应给出信号量 / 发任务通知，采样与计算交给任务做。
- **禁止忙等**。任务看门狗已启用并检查两个核的 IDLE 任务（5 秒超时）。若采样任务用轮询等 DRDY 而不是阻塞等待，会饿死 IDLE 触发复位。

### 总线分配

| 外设 | 建议 | 原因 |
|---|---|---|
| LCD（NV3030B，QSPI） | 用 ESP32-S3 的 LCD 外设 + `esp_lcd`（占用 **SPI2**），**不要用 GPIO 模拟** | 硬件加速，省 CPU；LVGL 官方有现成后端 |
| ADS131M02（SPI） | 独占 **SPI3** | 测量链路不能被其他设备插入事务打断。**为什么不是 SPI2**：SPI2 已被 LCD QSPI 占用，而 ESP32-S3 的一个 SPI 外设同一时刻只能映射一组引脚（LCD 在 IO15~18、ADC 在 IO9~12，无法共存）；「独占」的本意是"不被打断"，不是字面上的外设编号 |
| 触摸 CST816D | I2C（IO5/IO6） | 独立总线，避免与传感器互相阻塞 |
| AHT20 / SC7A20HTR | I2C（IO21/IO47） | 同一条总线，注意地址不冲突（0x38 / 0x19） |

### JTAG 占用

IO19/IO20 是原生 USB-Serial/JTAG 的数据线。**调试期间这两个脚不能挪作他用**。

### QSPI 面板命令封装（踩坑记录，改动显示驱动前必读）

本屏的 QSPI 命令字是 32 位，且**必须由驱动自己把 opcode 打包进去**：

```
线上 32bit 命令字 = (opcode << 24) | (cmd << 8)
     写命令 opcode = 0x02
     写显存 opcode = 0x32
```

关键事实：`esp_lcd` 对 32 位命令**只做字节序反转**（`spi_lcd_prepare_cmd_buffer()`），
**不会替你补 opcode**。所以直接 `esp_lcd_panel_io_tx_param(io, 0x2A, ...)` 发出去的是
`00 00 00 2A`，面板会当作无效命令全部忽略 —— 连 `SLPOUT(0x11)` / `DISPON(0x29)`
都不生效，表现为**整屏黑**（背光正常但没有任何内容）。

本工程曾因此长期黑屏。现在该屏统一走官方组件 `espressif/esp_lcd_spd2010`，
opcode 封装与约 200 条分页式初始化序列都由组件负责，**驱动里不要再手写命令表**。

另有两条相关约束：

- `spi_bus_config_t` 中 `mosi_io_num ≡ data0_io_num`、`miso_io_num ≡ data1_io_num`、
  `quadwp_io_num ≡ data2_io_num`、`quadhd_io_num ≡ data3_io_num` 共用**匿名联合**。
  QSPI 场景下只能写 `sclk_io_num / data0~3_io_num / max_transfer_sz`；
  若同时写了 `quadwp/quadhd = -1`，后写的会覆盖 `data2/data3`，
  等于只配了 2 根数据线（同样出不了图）。
- `esp_lcd` 的颜色数据传输是**异步**的（内部 `spi_device_queue_trans`，
  `spi_device_release_bus()` 不等 DMA 结束）。LVGL 适配层必须在
  `lcd_nv3030b_register_flush_done_cb()` 的回调里才调 `lv_display_flush_ready()`，
  否则 LVGL 会与 DMA 抢同一块绘图缓冲，表现为撕裂 / 花屏 / 整屏黑。

### LVGL 与显示子系统的引入方式

分工（详见 §1「外部依赖：统一由组件管理器托管」）：

| 组件 | 职责 |
|---|---|
| `lvgl/lvgl`（组件管理器托管） | LVGL 9.6 源码本体 |
| `components/drivers/lcd_nv3030b/` | QSPI 总线 + 面板（官方 `esp_lcd_spd2010`）+ 背光 + 导出 panel/io 句柄 |
| `components/drivers/touch_cst816d/` | 触摸读点（解析与厂商 STM32 参考例程一致） |
| `components/middleware/lvgl_port/` | 应用侧适配：字体（SPIFFS→PSRAM）、触摸 indev、首帧计数、旋转；LVGL 内核/任务/心跳/锁/显示由官方 `espressif/esp_lvgl_port` 提供 |
| `components/middleware/fs_storage/` | storage 分区挂为 `/spiffs`，字体镜像（`spiffs_create_partition_image`） |
| `components/services/ui/` | 启动页 + 测量/波形/设置三页（tileview/chart/roller 官方组件） |
| `components/services/power_measure/` | 4kHz 采样、块均值/功率/电能/容量（直流口径非 RMS）、三级波形缓冲、NVS 掉电保存 |

### 字体方案（OFL Noto Sans SC，文件系统存放）

- 字体**不嵌入 app**：`tools/fonts/make_fonts.py` 从 google/fonts 下载 Noto Sans SC
  可变 TTF，实例化 Regular(400)/Bold(700) 两个字重，按 **ASCII + GB2312 一级字库
  3755 字 + 全角标点/单位符号** 子集化，输出到
  `components/middleware/fs_storage/spiffs_image/fonts/`（约 1.1MB/个），构建时
  打入 SPIFFS 镜像随 `idf.py flash` 烧入 `storage` 分区（partitions.csv，7.5MB）。
- `lvgl_port_init`（启动表中位于 fs_storage 之后、测量任务启动之前）用标准
  `fopen` 把两个 TTF **整读入 PSRAM**，再用 `lv_tiny_ttf_create_data()` 建
  7 档字号 ×2 字重；之后渲染只走内存，**运行期对 flash 零访问**。
  这样避免了渲染中 SPIFFS 读（spi_flash 读会挂起 MSPI cache）与 NVS 擦除/
  LCD DMA 交错的 cache 故障窗口。文件缺失时回退内置 montserrat14（仅 ASCII）。
- **不要改回 `lv_tiny_ttf_create_file()` 边渲染边读**，除非重新评估 cache 安全。
- 子集扫描范围（v0.6.2 需求 3）：`make_fonts.py` 与 `tools/fonts/audit_glyphs.py`
  均扫描**整个 `components/`** 的字符串字面量（此前只扫 `services/ui/src`），避免
  "上游组件里定义、最终却显示到屏幕上"的文案漏字；内嵌网页 `web_console_page.c`
  例外——它的字符串由浏览器字体渲染，不经过设备字库（emoji/✔✘ 之类也进不了 CJK TTF）。
- NVS 定时擦除期间 4kHz DRDY 中断仍持续，故必须开 `CONFIG_GPIO_ISR_IRAM_SAFE`。

### 测量链路（4kSPS）

- ADS131M02 CLOCK 寄存器在 init 中显式写 **0x030E**（两通道使能 / HR / OSR=1024，
  恰为芯片复位值）并回读校验：f_MOD=4.096MHz，
  ODR=4.096MHz/1024=4000 SPS（寄存器位定义见 ads131m02.c 注释，出自手册 §8.6.4）。
  4kHz 稳定的前提是**读窗不跨越转换完成时刻**（手册 §8.5.1.5 的 DRDY 抑制机制，
  旧版 4MHz SPI + 任务调度路径下实测塌到 ~310Hz）：现以 20MHz SPI + DMA 把读窗
  压到 µs 级、固定在 DRDY 之后启动，DRDY→SPI 启动延迟 δ 由 ads131m02_get_stats()
  实测监控（M1 基线验收 p99≤20µs，详见 docs/ads131-4khz-implementation-plan.md）。
- Core1 采样任务由 DRDY 自节奏，每样本做标定换算并写 PSRAM 三级环形缓冲：
  tier0 4000Hz×4000（1s 窗）、tier1 50Hz×24000（480s，覆盖 60s 窗）、
  tier2 1.25Hz×6000（80min，覆盖 10min 窗）；
  每 200ms（墙钟，约 800 样本）输出一帧 V/I 块均值（直流口径）、平均功率与积分电能到 data_store。
- 温湿度/电池由 Core0 辅助任务 2s 采集一次，在块边界由采样任务合并进快照
  （维持 data_store 单写者）；电能/运行时间每 30s 落 NVS，启动恢复。

### 按键事件（状态机 + 主机单测）

`button` 驱动 10ms 采样 GPIO，语义判定全部在纯逻辑 `button_fsm.c`（消抖 20ms /
双击窗口 300ms / 长按 800ms），事件为单击/双击/长按开始/长按释放；该文件无平台
依赖，随 `test/host/run_host_tests.ps1` 在 PC 上做单元测试。esp_timer 回调只
向 UI 任务的消息队列投递事件，**回调内禁止碰 LVGL**。

**升级 LVGL 的步骤**（组件管理器托管方式）：

1. 改 `components/middleware/lvgl_port/idf_component.yml` 里的版本号（如 `lvgl/lvgl: "^9"`）；
2. `idf.py reconfigure`，组件管理器把新版本下载到 `managed_components/lvgl__lvgl/`
   并更新 `dependencies.lock`（lock 需入库）；
3. 打开新版本的 Kconfig，**逐项核对配置符号名是否变化**，同步改 `sdkconfig.defaults`；
   已有前车之鉴：9.6 把内存池从 `LV_MEM_SIZE_KILOBYTES`（KB）改成了 `LV_MEM_SIZE`（字节），
   沿用旧名会得到「DEPRECATED」且**不生效**；
4. 核对代码用到的 API 未变（`lv_display_*` / `lv_indev_*` 等）；
5. 删掉 `sdkconfig` 让它按 `sdkconfig.defaults` 重新生成，再全量编译。

**界面构建必须在 UI 任务里做**（`services/ui`），不能放在 `app_main` 的任务上：
`CONFIG_ESP_MAIN_TASK_STACK_SIZE` 只有 3584 字节，而 tileview 三页 + chart 等
界面构建会递归创建大量对象，很容易把主任务栈吃满导致溢出复位（LVGL 任务栈 16384 字节，
见 `lvgl_port.c` 的 `LVGL_PORT_TASK_STACK` 注释）。

---

## 10. 目录结构速查

```
main/                     应用入口与启动编排
components/
├── bsp/                  引脚定义（board_pins.h）与板级初始化
├── drivers/<外设>/       每外设一组件：CMakeLists.txt + include/ + src/
├── middleware/           事件总线、数据仓库、LVGL 适配、WiFi、网页
└── services/             状态机、测量、设备控制、配置、UI
managed_components/       组件管理器托管的外部依赖（不入库、由 lock 复现）
test/
└── host/                 纯逻辑主机测试
docs/                     架构与引脚文档
tools/                    构建 / 烧录 / 生成脚本
partitions.csv            分区表（双 4MB OTA + coredump + storage）
sdkconfig.defaults        配置基线（sdkconfig 本身不入库）
version.txt               固件版本号
```

---

## 11. 命名规范

| 类别 | 规范 | 示例 |
|---|---|---|
| 目录 / 组件 | 小写 snake_case，单数 | `ads131m02`、`battery_adc` |
| 组件内布局 | `include/` 对外，`src/` 实现与私有头 | `include/aht20.h` |
| 公共 API | `组件名_动作()` | `aht20_read()`、`buzzer_play()` |
| 类型 / 句柄 | `xxx_t` / `xxx_handle_t` | `power_sample_t` |
| 宏 | 全大写 + 组件前缀 | `AHT20_I2C_ADDR` |
| 引脚 | `BOARD_PIN_*` | `BOARD_PIN_BUZZER` |

代码风格由根目录 `.clang-format` 统一（4 空格缩进、100 列、指针右对齐、函数花括号独占一行）。

---

## 12. 后续里程碑（当前已知缺口）

测量链路、三页正式 UI、自动转屏均已实现；`wifi_manager`（STA / AP 配网 + 模式三档 +
扫描 + RSSI，v0.6.0/0.6.1）与 `web_console`（内嵌网页控制台，v0.6.0 起为
实时数据 / 波形图 / 设置 / API / 日志五页签，10Hz 推送 + WiFi 扫描配网）已落地。
v0.6.2 起 web_console 增加 `GET /api/config`（回读配置）与 `GET /api/info`
（版本 / IP / MAC / 运行时间 / 内存水位）；设备端新增"无操作 15/30/60s 背光降到 10%"
的自动降亮（触摸与按键均重置计时并恢复亮度）。
以下为**尚未实现**的缺口：

| 缺口 | 现状 | 影响 |
|---|---|---|
| 波形 120s 窗稀疏度 | 120s 窗映射 tier2（降采样最深，桶内源点不足） | 已列入路线图：改 tier1 桶聚合，修复包络失效 |
