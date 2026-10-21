# XT60 功率计（XT60 Power Meter）

基于 **ESP32-S3 + LVGL** 的开源便携功率计固件：XT60 接口直插测量直流电压 / 电流 / 功率 / 电能，
24bit 同步采样（ADS131M02，4 kSPS），圆形 IPS 触摸屏实时显示测量值与示波器风格波形。

> 面向想学习「ESP-IDF + LVGL 实战」的开发者：代码按分层架构组织，
> 注释详细讲解设计取舍与 ESP-IDF / FreeRTOS API 的用法，适合入门者跟随学习。

---

## 功能特性

- **测量**：电压 / 电流块均值（直流口径，约 200ms 窗平均，非 RMS）、瞬时功率、平均功率、积分电能（Wh）、容量（Ah）、运行时间
  - 24bit ΔΣ ADC（ADS131M02）双通道同步采样，4 kSPS，CRC 帧校验剔除损坏数据
  - 标定换算（增益 + 零偏），支持 UI 校准向导与**空载一键清零**
  - **空载小信号死区**：|I| < 2 mA 时读数 / 峰谷 / 告警 / 电能一律按 0 处理，
    消除 1 mΩ 分流器固有噪声（单帧 σ≈2 mA）造成的末位跳动与电能虚假累积；
    波形页仍显示原始样本，mA 级变化照常可见
  - 电能 / 运行时间每 30s 定时写 NVS，掉电恢复
  - **过温保护（恒开，双数据源）**：主控 SOC 结温与 AHT20 机内温度**任一超过 65℃**
    即告警——弹窗（同时显示两路温度）+ 蜂鸣 + 网页红色横幅；与过压/过流/过功率
    （可关）不同，此项属硬件保护，不可关闭
- **波形页**（示波器风格）：
  - 双轴折线 + 自动量程（1-2-5 标准格值序列，刻度锚定整格值，可直接心算读数；
    刻度去尾零：3.00V→3V、0.200A→0.2A，读数更干净）
  - 时间窗 1s / 10s / 30s / 60s / 120s / 10min，三级降采样环形缓冲
  - 黑底亮线配色、min/max 包络渲染（防混叠丢峰）、量程滞回（防跳变）
  - 底部实时 / 平均数据行：定宽列布局，数值跳动不移位
- **UI**：测量 / 波形 / 设置三页滑动（首末页到头即止，不循环），触屏 + 纯按键双操作，快捷设置面板
  - 首页电能数据条可点击，在 Wh / Ah 之间切换显示
  - 快速菜单的「清除电能统计数据」**二次确认**（v0.6.8，与设置页同一共享确认框）
  - 支持横屏 0° + 90° / 180° / 270° 四方向（重力自动旋转 + 手动角度）
  - 自动降亮：无操作 15 / 30 / 60 秒（可设）后背光降到 10%，触摸或按任意键立即恢复
  - 设置子页的亮度/响度滑条与降亮档位会**实时跟随**网页控制台的远程改动（无需退出重进）
  - 启动页进度条 + 外设自检，故障弹窗提示
- **WiFi 配网**（v0.6.0 起）：
  - 工作模式三档：自动（有凭据连路由器 / 无凭据开热点）、仅 STA、强制配网热点
  - 配网热点为 **WPA2-PSK 加密**，设备端 WiFi 设置页直接显示「热点名 + 密码」，按提示输入即可连接
  - 设备端「搜索 WiFi」独立页：扫描附近热点 → 点选 → 虚拟键盘输密码连接；忘记网络一键清除凭据
    （「重新扫描」按钮置于标题栏右侧，结果列表一屏可见 3~4 行，可上下滑动）
  - 状态栏 WiFi 扇形图标随状态变化：已连接按 RSSI 四档配色（绿/绿/黄/红）、
    连接中黄、热点模式蓝 + AP 徽标、未连接灰 + 斜杠
- **联网与网页控制台**（v0.5.0 起）：
  - WiFi STA / AP 双模：设备热点配网 → 连上路由器后以 IP 提供 HTTP 服务；凭据写入 NVS 掉电保留
  - 网页控制台五页签：**实时数据 / 波形图 / 设置 / API / 日志**，数值小数位与设备屏显完全一致
  - 实时数据按信息层级排版：功率主读数 → 电压/电流/功率 → 累计量 → 电源与链路 → 环境结温
    - 电压/电流/功率三卡片**分色**（绿/蓝/橙，与波形图三通道同色）
    - **读数刷新帧率可设**（15/10/5/2/1 Hz）：低于推送速率时对收到多帧做平均滤波，
      读数更稳；设备发送速率与波形缓冲不受影响
  - 网页波形图（Canvas 2D）：V / I / P 三通道叠加、纵 5 格 / 横 10 格网格、单格最小值 1V / 0.1A
    （格子锚定 1 / 2 / 5 / 10 标准序列）、1s~10min 时间窗、暂停、拖动回看历史、十字光标读数、
    锁定量程、通道统计卡（当前/最小/最大/平均分色）、波形 CSV 导出
  - 设置页支持 WiFi 搜索选点配网、模式切换与数值滑动条，**打开页面即从设备读取
    当前设置回填表单**（不会显示前端默认值）
  - **测量设置（标定）**：网页可直接读写电压/电流的增益与零偏；
    「电流零点校准」在空载下采样 **3 秒块均值**自动校零（与设备端按钮同一实现）
  - 实时数据页提供「清除电能统计」按钮（二次确认，测量核块边界生效）
  - **API 页**：编号卡片式接口文档（方法/路径/参数徽标 + 字段说明 + 错误码约定），
    每张卡片带「▶ Test」就地发真实请求并显示状态码、耗时与响应正文
  - 接口：`/api/state`（快照，含 WiFi 状态/RSSI）+ WebSocket `/ws`（15Hz 推送）、
    `GET/POST /api/config`（回读 / 下发参数与标定，含 wifi_mode/wifi_forget）、
    `/api/info`（版本 / IP / MAC / 运行时间 / 内存水位）、`/api/wifi/scan`（热点扫描）、
    `/api/export.csv`（分钟级历史导出）、HTTP OTA 升级；PC / 移动端自适应
- **其他外设**：温湿度（AHT20）、电池电压监测、芯片内部温度传感器（SOC 结温）、
  加速度计自动转屏（SC7A20H）、蜂鸣器（按键音 / 提示音）、状态指示灯（分级故障指示）
- **工程化**：双 OTA 分区 + A/B 升级回滚（HTTP 上传 + 启动回滚检查）、崩溃转储（coredump）取证、
  PC 端 LVGL UI 模拟器（烧录前走查 / 回归截图）、主机端纯逻辑单元测试
  - 性能（v0.6.3）：巡检与引擎日志合并且降噪（实测 2.6 → 0.8 行/秒，减少 Core0 同步串口占用）、
    网页 WebSocket 推送改用常驻缓冲（去掉每帧堆分配）、LVGL 心跳 5ms → 16ms、
    渲染分块 16 → 24 行、LVGL/UI 任务栈由 PSRAM 移回内部 RAM
  - 核分工（v0.6.4）：orientation / web_console_ws / sys_monitor 三个低优先级周期任务迁至
    Core1（实测 drdy 4000Hz 与 drop 计数无变化），Core0 专注交互与通信

## 硬件概要

| 项 | 型号 / 参数 |
|---|---|
| 主控 | ESP32-S3-WROOM-1-**N16R8**（16MB Flash + 8MB 八线 PSRAM） |
| 屏幕 | NV3030B 240×284 IPS（QSPI 4bit），CST816D 电容触摸 |
| 计量 ADC | TI ADS131M02（24bit 双通道同步采样 ΔΣ，SPI3） |
| 电流采样 | 1 mΩ / 5W 四端开尔文分流器，PGA 16x |
| 电压采样 | 101:1 精密分压（±0.1%），PGA 1x |
| 传感器 | AHT20 温湿度、SC7A20HTR 三轴加速度计 |
| 电源 | XT60 输入（SY8511B 3.3V Buck）/ USB / 锂电池（TP4057 充电） |

完整引脚分配见 [docs/hardware_pinout.md](docs/hardware_pinout.md)。

## 快速开始

### 环境准备

1. 安装 [ESP-IDF v5.5 及以上](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/)
   （本工程开发基线为 v6.1，Windows 下建议用 Espressif 官方安装器）；
2. 克隆本仓库：

```powershell
git clone https://gitee.com/zeruns/xt60-power-meter.git
cd xt60-power-meter/02-Software/XT60-Power-Meter-Firmware
```

> 第三方依赖（LVGL 9.6、esp_lvgl_port、esp_lcd_spd2010）由 **ESP 组件管理器**自动下载
> （版本锁定在 `dependencies.lock`），无需手动下载。首次构建需联网。

### 构建

Windows 一键脚本（内部会设置 `IDF_PATH` 并加载 `idf.py` 环境）：

```powershell
.\build_fw.bat
```

或手动（已加载 IDF 环境的终端）：

```powershell
idf.py build
```

### 烧录

```powershell
.\flash_fw.bat          # 默认 COM3，内部使用 idf.py -p COM3 flash
.\flash_fw.bat COM5     # 串口从第一个参数取
```

非 COM3 时手动指定串口：

```powershell
idf.py -p COMx flash
idf.py -p COMx monitor   # 查看串口日志（115200）
```

烧录速率可设环境变量 `ESPBAUD`（约定 921600）：`$env:ESPBAUD="921600"`。

**固件由 5 个镜像组成，各烧到固定偏移**（`idf.py flash` 会按 `build/flash_args` 自动处理）：

| 文件 | 偏移 | 作用 |
|---|---|---|
| `bootloader.bin` | `0x0` | 二级引导程序 |
| `partition-table.bin` | `0x8000` | 分区表 |
| `ota_data_initial.bin` | `0xf000` | OTA 槽位初始选择 |
| `XT60-Power-Meter-Firmware.bin` | `0x20000` | 应用固件（代码 + 内嵌网页，约 1.5MB） |
| `storage.bin` | `0x830000` | SPIFFS 字体镜像（中文字体约 4.2MB） |

> **改字库必须全量烧录**：字体在独立的 `storage` 分区，**OTA 只更新 app**，
> 只走 OTA 会出现"升级成功但中文仍显示方框"。
>
> 不装 ESP-IDF 也可以用网页工具 **ESPConnect**（Chrome/Edge + Web Serial）烧录，
> 支持单文件 `XT60-Power-Meter-full.bin`（偏移 `0x0`）或按上表分文件烧。
> 详细步骤、参数与排错见 **[docs/build_and_flash.md](docs/build_and_flash.md)**。

## 目录结构

```
main/                     应用入口与启动编排（组合根：main.c / app_init.c / app_tasks.c）
components/
├── bsp/                  板级支持：引脚唯一真相源（board_pins.h）
├── drivers/<外设>/       设备驱动层，每外设一个组件
│   ├── ads131m02/        24bit 计量 ADC（SPI3 + DRDY 中断，CRC 帧校验）
│   ├── lcd_nv3030b/      QSPI LCD（官方 esp_lcd_spd2010 组件）+ 背光
│   ├── touch_cst816d/    电容触摸（I2C）
│   ├── aht20/ imu_sc7a20h/ battery_adc/ buzzer/ status_led/ key_button/
├── middleware/           中间件：event_bus、data_store（seqlock）、lvgl_port、
│                         fs_storage（SPIFFS 字体）、wifi_manager（STA/AP 配网）、
│                         web_console（内嵌网页控制台：HTTP + WebSocket + OTA）
└── services/             业务服务：power_measure（4kSPS 测量链路）、system_state、
                          settings（NVS 持久化）、orientation（自动转屏）、ui（三页界面）
managed_components/       组件管理器托管依赖（不入库，由 lock 复现）
docs/                     架构设计与引脚文档
tools/
├── ui_sim/               PC 端 LVGL UI 模拟器（走查 + 回归截图）
├── fonts/                中文字体生成脚本（Noto Sans SC 子集化）
└── ui_preview/ mcp/      辅助工具
test/host/                纯逻辑主机端单元测试
partitions.csv            分区表：双 4MB OTA + 64KB coredump + 8MB storage
sdkconfig.defaults        配置基线（sdkconfig 不入库，克隆后自动生成）
```

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | **固件架构**：分层结构、状态机、启动容错、双核分工、seqlock、QSPI 踩坑记录、测量链路 |
| [docs/build_and_flash.md](docs/build_and_flash.md) | **固件组成与烧录**：5 个镜像各自作用/偏移、分区表、`idf.py` / 网页 ESPConnect 两种烧法、OTA 限制与排错 |
| [docs/hardware_pinout.md](docs/hardware_pinout.md) | **引脚分配表**：GPIO 映射、总线分配、Strapping 约束、中断与内存约束 |
| [tools/ui_sim/README.md](tools/ui_sim/README.md) | **UI 模拟器**：PC 上运行真机同源界面，烧录前走查与回归 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | **贡献指南**：开发环境、构建烧录、UI 回归、代码规范与提交规范 |

**第一次接手本工程？** 建议阅读顺序：本文档 → `docs/hardware_pinout.md`（知道硬件长什么样）→
`docs/architecture.md`（知道代码为什么这样组织）→ `main/main.c`（入口）→
`components/services/power_measure/`（核心测量链路）。

## 开发约定（摘要）

- **分层依赖只能自上而下**：`main → services → middleware → drivers → bsp`；
  业务代码禁止直接调用裸外设 API（`gpio_*` / `spi_*` / `i2c_*`）。
- **引脚编号只允许出现在 `components/bsp/include/board_pins.h`**，其他文件引用 `BOARD_PIN_*` 宏。
- 命名规范与代码风格见 [docs/architecture.md §10-11](docs/architecture.md)（`.clang-format` 统一格式）。
- 提交信息遵循 **中文 Conventional Commits**：`type(scope): 中文标题` + 中文正文。
- **版本号唯一来源 `version.txt`**（编译进 app_desc），发版时打 git tag 与之对应。
- CI 本地入口 `tools/ci/run_ci.ps1`：一键跑构建 + 主机单测 + UI 回归并汇总结果。
- UI 改动请先跑 `tools/ui_sim` 模拟器验证再烧录实机：

```powershell
powershell -ExecutionPolicy Bypass -File tools/ui_sim/build.ps1
```

## 常见问题

- **首次编译报组件下载失败**：需联网让组件管理器拉取 `managed_components/`（版本由
  `dependencies.lock` 锁定）；公司网络受限时可配置镜像源。
- **烧录后黑屏但背光亮**：先确认烧录的是完整固件（含 bootloader/partition-table）；
  仍异常时按 [docs/architecture.md §9](docs/architecture.md) 的 QSPI 命令封装一节排查。
- **测量值恒为 0 / 乱码**：核对 ADS131M02 时钟源（R27/R29 二选一，本工程默认板载晶振），
  详见 `components/drivers/ads131m02/src/ads131m02.c` 文件头的「上板调试要点」。
- **崩溃了怎么取证**：coredump 分区已启用，panic 后延时 3 秒复位（留时间打印日志）；
  分析方法见 [docs/architecture.md §8](docs/architecture.md)。

## 参与贡献

欢迎 Issue 与 Pull Request！完整流程见 [CONTRIBUTING.md](CONTRIBUTING.md)，提交前请：

1. 遵循上述开发约定（分层、引脚、命名、中文 Conventional Commits）；
2. UI 改动附上 `tools/ui_sim` 截图验证结果；
3. 拆分原子提交（一个提交只做一件事）。

## 许可证

本固件采用 [MIT 许可证](LICENSE)。第三方组件及其许可见 [THIRD_PARTY.md](THIRD_PARTY.md)
（含 LVGL、esp_lvgl_port 等）。字体 Noto Sans SC 采用 [SIL Open Font License 1.1](https://openfontlicense.org/)。
