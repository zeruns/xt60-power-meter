# XT60 功率计（XT60 Power Meter）

基于 **ESP32-S3 + LVGL** 的开源便携功率计固件：XT60 接口直插测量直流电压 / 电流 / 功率 / 电能，
24bit 同步采样（ADS131M02，4 kSPS），圆形 IPS 触摸屏实时显示测量值与示波器风格波形。

> 面向想学习「ESP-IDF + LVGL 实战」的开发者：代码按分层架构组织，
> 注释详细讲解设计取舍与 ESP-IDF / FreeRTOS API 的用法，适合入门者跟随学习。

---

## 功能特性

- **测量**：电压 / 电流真有效值（RMS）、瞬时功率、平均功率、积分电能（Wh）、运行时间
  - 24bit ΔΣ ADC（ADS131M02）双通道同步采样，4 kSPS，CRC 帧校验剔除损坏数据
  - 标定换算（增益 + 零偏），支持 UI 校准向导与**空载一键清零**
  - 电能 / 运行时间每 30s 定时写 NVS，掉电恢复
- **波形页**（示波器风格）：
  - 双轴折线 + 自动量程（1-2-5 标准格值序列，刻度锚定整格值，可直接心算读数）
  - 时间窗 1s / 10s / 30s / 60s / 120s / 10min，三级降采样环形缓冲
  - 黑底亮线配色、min/max 包络渲染（防混叠丢峰）、量程滞回（防跳变）
  - 底部实时 / 平均数据行：定宽列布局，数值跳动不移位
- **UI**：测量 / 波形 / 设置三页循环滑动，触屏 + 纯按键双操作，快捷设置面板
  - 支持横屏 0° + 90° / 180° / 270° 四方向（重力自动旋转 + 手动角度）
  - 启动页进度条 + 外设自检，故障弹窗提示
- **其他外设**：温湿度（AHT20）、电池电压监测、加速度计自动转屏（SC7A20H）、
  蜂鸣器（按键音 / 提示音）、状态指示灯（分级故障指示）
- **工程化**：双 OTA 分区已就绪（A/B 升级回滚通道**规划中**，wifi_manager / web_console
  目前为接口骨架）、崩溃转储（coredump）取证、
  PC 端 LVGL UI 模拟器（烧录前走查 / 回归截图）、主机端纯逻辑单元测试

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
│                         fs_storage（SPIFFS 字体）、wifi_manager / web_console（接口骨架，规划中）
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
