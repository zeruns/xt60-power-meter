# XT60 功率计 · 基于 ESP32-S3 的彩屏电压/电流/功率表

> XT60 接口直插测量直流电压、电流、功率与电能，1.83 寸 IPS 彩屏实时显示读数和波形；
> 内置锂电池可充电，支持 WiFi 网页控制台。**硬件、固件、外壳全开源。**

[![License: CERN OHL-S v2](https://img.shields.io/badge/hardware-CERN%20OHL--S--2.0-blue)](LICENSE)[![Firmware: MIT](https://img.shields.io/badge/firmware-MIT-green)](02-Software/XT60-Power-Meter-Firmware/LICENSE)![MCU](https://img.shields.io/badge/MCU-ESP32--S3-orange)![ADC](https://img.shields.io/badge/ADC-ADS131M02%2024bit-yellow)![Firmware](https://img.shields.io/badge/firmware-v0.6.9-brightgreen)

> 🎬 **演示视频**：待补充（B 站链接）
> 📦 **立创开源硬件平台工程**：待补充
> 🔗 **仓库地址**：[https://gitee.com/zeruns/xt60-power-meter](https://gitee.com/zeruns/xt60-power-meter)
> 💬 电子交流社区：[https://bbs.eeclub.top/](https://bbs.eeclub.top/)
> 📖 作者博客：[https://blog.zeruns.com/](https://blog.zeruns.com/)
> 🇬🇧 **English**: [README.en.md](README.en.md)



![](https://tc2.zeruns.com/Pic/2026/10/IMG_20261031_163238%281%29_f6cebf8d83645903a56acc390aabcf88.webp)



---



## 目录

- [一、项目简介](#一项目简介)
  - [功能亮点](#功能亮点)
  - [主要参数](#主要参数)
- [二、成品展示](#二成品展示)
  - [2.1 外观](#21-外观)
  - [2.2 内部结构](#22-内部结构)
  - [2.3 屏幕界面](#23-屏幕界面)
  - [2.4 网页控制台](#24-网页控制台)
- [三、硬件设计](#三硬件设计)
  - [3.1 总体架构](#31-总体架构)
  - [3.2 电源与电池管理](#32-电源与电池管理)
  - [3.3 电压采样](#33-电压采样)
  - [3.4 电流采样](#34-电流采样)
  - [3.5 主控、显示与传感器](#35-主控显示与传感器)
  - [3.6 PCB 设计](#36-pcb-设计)
- [四、软件设计](#四软件设计)
  - [4.1 固件架构](#41-固件架构)
  - [4.2 测量链路](#42-测量链路)
  - [4.3 本地界面](#43-本地界面)
  - [4.4 网页控制台与 API](#44-网页控制台与-api)
  - [4.5 工程化](#45-工程化)
- [五、外壳与组装](#五外壳与组装)
- [六、制作与复现](#六制作与复现)
  - [6.1 物料与成本](#61-物料与成本)
  - [6.2 打板与钢网](#62-打板与钢网)
  - [6.3 焊接](#63-焊接)
  - [6.4 烧录](#64-烧录)
  - [6.5 校准](#65-校准)
- [七、使用说明](#七使用说明)
- [八、待实现功能](#八待实现功能)
- [九、目前存在的问题](#九目前存在的问题)
- [十、仓库结构](#十仓库结构)
- [十一、文档索引](#十一文档索引)
- [十二、参与贡献](#十二参与贡献)
- [十三、许可证](#十三许可证)
- [十四、免责声明](#十四免责声明)
- [推荐阅读](#推荐阅读)



---



## 一、项目简介

做航模、无人机、电池充放电测试时，经常需要一台能**直接插在 XT60 回路里**的功率计：
既要看得清大电流下的电压跌落，也要能捕捉插拔瞬间的浪涌波形，最好还能把数据同步到电脑上。
市面上的产品要么屏幕小、要么没有波形、要么数据只能自己看，于是有了这个项目。

XT60 功率计以 **ESP32-S3** 为主控，采用 **TI ADS131M02 24bit 双通道同步采样 ADC（4 kSPS）**，
配合 1 mΩ 四端开尔文分流器与 101:1 精密分压，测量 **0~90V、额定 ±30A（峰值 ±75A）** 的直流回路；
1.83 寸 IPS 彩屏运行 **LVGL 9** 界面，提供测量、波形、设置三页；
WiFi 连上路由器后可用浏览器打开**网页控制台**，实时看数据、看波形、改设置、导 CSV，
也提供完整的 HTTP API 方便自己写上位机或自动化脚本。



### 功能亮点

| 分类 | 功能 |
| :-: | :-- |
| 测量 | 电压 / 电流平均值（直流口径）、瞬时与平均功率、累计电能（Wh）、容量（Ah）、运行时间 |
| 采样 | ADS131M02 24bit 双通道同步采样 4 kSPS，CRC 帧校验自动剔除坏帧 |
| 波形 | 示波器风格双轴折线，V/I/P/E 多模式，1s ~ 10min 六档时间窗，自动量程（1-2-5 标准格值） |
| 交互 | 电容触屏 + 双按键，首页上拉快速菜单，三页滑动，重力感应自动旋转屏幕方向，长时间自动降亮度 |
| 告警 | 过压（上限 90V）/ 过流（上限 70A）/ 过功率（上限 6300W）阈值可设 + 过温保护（双温度源，≥65℃ 恒开） |
| 联网 | WiFi STA/AP 配网，网页控制台五页签（实时/波形/设置/API/日志），15Hz WebSocket 推送 |
| 电源 | XT60 输入 / USB / 锂电池三路供电，USB 与电池经 TPS2116 自动优先切换，TP4057 充电 |
| 工程 | 双 OTA 分区 + 回滚、coredump 取证、PC 端 LVGL UI 模拟器、主机端单元测试 |

### 主要参数

| 项目 | 参数 |
| --- | --- |
| 主控 | ESP32-S3-WROOM-1-N16R8（16MB Flash + 8MB 八线 PSRAM，双核 240MHz） |
| 计量 ADC | TI ADS131M02，24bit Σ-Δ，双通道同步采样，4 kSPS |
| 电压量程 | 设计测量 **0~90V**（分压链路理论满量程 ±121V，受辅助电源最高输入 100V 限制取 90V） |
| 电流量程 | 峰值 **±75A**（1mΩ + PGA 16x 的 ADC 满量程）；额定 **±30A**（5W 分流器功率上限 70A，XT60 接口额定 30A） |
| 接口 | XT60（额定 30A；复刻者可自行重画底板换 XT90，以发挥分流器 70A 能力） |
| 显示 | 1.83 寸 IPS，240×284（横屏 284×240），NV3030B 控制器，QSPI；CST816D 电容触摸 |
| 传感器 | SC7A20HTR 三轴加速度计（自动转屏）、AHT20 温湿度 |
| 电源 | XT60 输入（SY8511BADC 3.3V/1A）、USB/电池（TLV62084 3.3V/2A）、USB 与电池经 TPS2116 自动优先切换、TP4057 400mA 充电 |
| 通信 | WiFi 2.4G（STA/AP），HTTP + WebSocket |
| 外壳 | SolidWorks 源文件 + 3MF 打印文件（上盖 / 中框 / 底盖） |

> ⚠️ 本机适用于爱好者测量与调试，**不作为计量器具**；2mA 以下电流按 0 显示（小信号死区），
> 波形页仍可观察 mA 级细节。详细说明见[使用说明](#七使用说明)。
>
> 切换到XT60输入供电时最低输入电压5V才能启动。
>
> 软件完全由AI开发。



---



## 二、成品展示

### 2.1 外观

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_040258_3d78c7efd72f1373efcd3bacb8796a8f.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_040311_711ff4b0826e90d076c9959e9fb6f459.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_040708_33f38b9f22f6c5d8b28026537e9d76ab.webp)

机身采用 3D 打印外壳，上盖、中框、底盖三件式结构，螺丝固定；

正面为 1.83 寸 IPS 屏，屏幕左侧是系统状态指示灯与复位按键，机身设有两颗实体按键（K1/K2），上滑屏幕或长按 K2 可呼出快速菜单。

Type-C口右侧开关可切换供电（电池&USB 或 XT60）。



### 2.2 内部结构

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261021_225034_b2862a188c01ba6d2c8e752f381a37b5.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261021_231338_754f05b2abf53e373bfc09801c68e31f.webp)

整机为**上下两块 PCB** 叠层结构：上板负责主控、显示、传感器与按键、电压分压与电源管理，下板负责 XT60 输入输出、1mΩ 分流器。电池位于上板背面，夹在上下板中间，可通过双面胶/泡棉固定，建议包上青稞纸。



### 2.3 屏幕界面

| 测量主界面 | 示波器风格波形页 |
| :---: | :---: |
| ![测量页](https://tc2.zeruns.com/Pic/2026/11/ui_measure_8ab2c86641b538251224d69956f2779e.png) | ![波形页](https://tc2.zeruns.com/Pic/2026/11/ui_wave_1a829869dd821102645d119c8c5ea365.png) |
| 电压 / 电流 / 功率四行大数字，底部电能、温湿度、峰谷值 | V/I/P 多模式双轴曲线，1s~10min 时间窗，自动量程 |

| 设置页 | 快速菜单（上拉） |
| :---: | :---: |
| ![设置页](https://tc2.zeruns.com/Pic/2026/11/ui_settings_33b8b3b590df65d29e27988189f20f94.png) | ![快速菜单](https://tc2.zeruns.com/Pic/2026/11/ui_quick_menu_a5f4a9ac0d1ef87bb35d6ec92ffd0f88.png) |
| 测量设置、显示与声音、WiFi、告警、电能统计等二级页 | 清除电能统计（二次确认）、数据刷新率 1~30Hz |

> 以上界面图来自项目自带的 PC 端 LVGL 模拟器（`tools/ui_sim`），与实机渲染一致；



### 2.4 网页控制台

![](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-10-25_01-59-40_84cf88d80e3008780c243f9abd56fd26.webp)

![](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-10-25_01-58-54_d3a2d4055abff141ace6b126fe2e7eaf.webp)

![](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-10-25_02-01-40_39f49c40728c179b65d2cce257b132bc.webp)

手机 / 电脑浏览器访问设备 IP 即可打开控制台，无需安装 App；断网时设备本地界面不受影响。



---



## 三、硬件设计

### 3.1 总体架构

![XT60 功率计硬件系统框架图](https://tc2.zeruns.com/Pic/2026/11/block-diagram_4e1dbc1b78441e2d29679fb0b7d7bc6f.webp)

整机由六个部分组成：

| 模块 | 主要器件 | 职责 |
| :-: | --- | --- |
| 主控 | ESP32-S3-WROOM-1-N16R8 | 采样调度、UI 渲染、WiFi 通信、数据存储 |
| 计量前端 | ADS131M02 + LRA8551A | 电压/电流同步采样，24bit，4 kSPS |
| 显示交互 | NV3030B LCD + CST816D 触摸 + 双按键 + 蜂鸣器 | 本地显示与操作 |
| 传感器 | SC7A20HTR + AHT20 | 屏幕方向、机内温湿度 |
| 电源 | SY8511BADC / TLV62084 / TPS2116 / TP4057 | 三路输入切换、3.3V 稳压、电池充电 |
| 存储/升级 | 片内 16MB Flash + 双 OTA 分区 | 固件、字体资源、OTA 升级与回滚 |



### 3.2 电源与电池管理

![电源管理原理图](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-11-06_22-22-09_cc631dd51b0ae752e5db4439c9ec5341.webp)

- **三路供电**：XT60 被测回路、USB-C、单节锂电池；
  XT60 输入经 SY8511BADC 降压到 3.3V（最大 1A），USB/电池经 TLV62084 降压到 3.3V（最大 2A），
  TPS2116 在 USB与电池之间做优先级切换（优先 USB），具体合并方式见原理图；
- TP4057 负责锂电池充电（400mA），电池电压经 1:1 分压进ESP32的ADC，用于电量显示；
- 3.3V 轨为 MCU、LCD、ADC 数字侧与传感器供电，模拟前端供电与滤波细节见原理图。

USB与电池之间优先级切换这部分电路除了TPS2116芯片的方案外（V2版），还做了一个用分立器件（MOS）实现的版本（V1版）。

![](https://tc2.zeruns.com/Pic/2026/11/image-20261106222543910_7ae153e742438159b5e1acf7ae4ad9b7.png)



### 3.3 电压采样

![电压采样原理图](https://tc2.zeruns.com/Pic/2026/11/image-20261106222831117_acf0e4e363f391a0d4f6b7ba33af24c9.png)

电压通道采用 **101:1 精密分压**（100kΩ / 1kΩ，±0.1% 薄膜电阻），
分压后经零漂移运放 LRA8551A 缓冲，再进入 ADS131M02 的 CH1（PGA 1x）。
分压链路理论满量程 121V，但整机辅助电源最高输入为 100V，因此**设计测量范围取 0~90V**；
按该量程折算，单 LSB 约 14.5µV。



### 3.4 电流采样

![分流器与电流采样](https://tc2.zeruns.com/Pic/2026/11/image-20261106222918309_e099710a3e10ec60d1fa0de043bf32b6.png)

![电流采样原理图](https://tc2.zeruns.com/Pic/2026/11/image-20261106223640128_f7bae675cfd244da20d27731f7d01640.png)

电流通道使用 **1mΩ / 5W 四端开尔文分流器**，电压引出点独立于大电流走线，
避免铜箔电阻引入误差；信号经RC滤波后直接进入 ADS131M02 CH0（PGA 16x）。
PGA 16x 下 ADC 满量程对应 **±75A 峰值**（1 LSB≈8.94µA）；受 5W 分流器功率限制，额定最大 70A
（70A 时压降 70mV、功耗 4.9W）；XT60 接口额定 30A（30A 时压降 30mV、功耗 0.9W）。
需要长期测更大电流的复刻者，可自行重画底板换 **XT90** 接口。

> 为什么用 1mΩ：要覆盖 ±75A 峰值、额定 30A 的电流，又要把分流器自身发热控制在 5W 以内；
> 代价是小信号噪声较大（单帧 σ≈2mA），固件用 200ms 块均值 + 2mA 死区解决空载跳动。



### 3.5 主控、显示与传感器

![主控与显示原理图](https://tc2.zeruns.com/Pic/2026/11/%E4%B8%BB%E6%8E%A7%E4%B8%8E%E6%98%BE%E7%A4%BA%E5%8E%9F%E7%90%86%E5%9B%BE-1793977952919-14_6701c9fae2ac87c8ed0260deb9e7d04c.webp)

- ESP32-S3 通过 **QSPI**（4 数据线）驱动 NV3030B 屏幕；TE 引脚已引出但当前固件未使用，
  刷新由 LVGL 分块 flush 控制；
- CST816D 电容触摸走 I2C（0x15），INT 接 GPIO46；
- AHT20（0x38）与 SC7A20HTR（0x19）共用传感器 I2C 总线；
- 双按键 K1（BOOT）/K2 低电平有效，蜂鸣器由 GPIO1 经 AO3400A 低边驱动；
- 状态指示灯 IO39 低电平点亮，用于启动/故障分级提示。



### 3.6 PCB 设计

![上板PCB](https://tc2.zeruns.com/Pic/2026/11/IMG_0872_22d390e02a0eba572fcd520400ed8f82.webp)

![上板PCB底层](https://tc2.zeruns.com/Pic/2026/11/IMG_0890_ab74e525497544224c9c0a947214686a.webp)

![上板PCB顶层](https://tc2.zeruns.com/Pic/2026/11/IMG_0891_a087c249043ec4415c66ad58e572ae30.webp)

![下板PCB](https://tc2.zeruns.com/Pic/2026/11/IMG_0903_046bb89dd6296045676408ae1e18d249.webp)

设计要点：

- 上下板叠层，板间用排针/排母连接，屏幕与各种芯片走上板，大电流路径走下板；
- 电流采样是差分小信号，ISENSE 走线尽量短，并远离 DC-DC 电感与开关节点；
- ADC 的 SPI 与 DRDY 走线短，避免与时钟线、开关噪声长距离并行；
- 大电流路径走线加粗，降低大电流（额定 30A、峰值 75A）下的压降与温升；
- 模拟前端与数字部分的地/电源处理见原理图与 PCB 工程文件。

> PCB 工程、Gerber 与交互式 BOM 见 `05-生产文件/`；原理图 PDF 见 `01-Hardware/`。



---



## 四、软件设计

### 4.1 固件架构

固件基于 **ESP-IDF v6.1 + LVGL 9.6**，按「驱动层 / 中间件 / 服务层 / 应用层」分层：

```
main/                 启动流程、任务创建、参数下发（app_tasks）
components/
├── drivers/          ads131m02 / lcd_nv3030b / key_button / buzzer / imu_sc7a20h ...
├── middleware/       data_store（快照）/ web_console / wifi_manager / lvgl_port / fs_storage
└── services/         power_measure（测量核心）/ ui（界面）/ settings / orientation / system_state
```

双核分工（ESP32-S3 两个 Xtensa 核）：

- **Core 1（测量核）**：`task_sample` 由 DRDY 中断驱动，4 kSPS 读帧、CRC 校验、码值换算、
  波形环形缓冲、200ms 块统计与电能积分；`task_aux` 负责温湿度/电池等慢速采集；
- **Core 0（交互核）**：LVGL 渲染、按键事件、WiFi/HTTP 服务与网页推送。

### 4.2 测量链路

1. ADS131M02 每 250µs 产生一次 DRDY，SPI DMA 读取 4 字帧（STATUS/CH0/CH1/CRC）；
2. CRC-16/CCITT 校验通过后，按标定参数换算为电压 V、电流 A、瞬时功率 W；
3. 原始样本写入 PSRAM 三级环形缓冲，供波形页回看；
4. 每 200ms（墙钟）结算块均值、平均功率、峰谷值与告警，并积分电能/容量；
5. 每 25ms 产出一帧 40Hz 显示快照到 `data_store`，UI 与网页都只读快照。

> 细节与踩坑记录（4kHz 稳定读取、CRC 毛刺、内存策略、软件旋转等）见
> [架构设计文档](02-Software/XT60-Power-Meter-Firmware/docs/architecture.md)。

### 4.3 本地界面

- 三页 `tileview`：测量 / 波形 / 设置，首末页到头即止；
- 全功能支持**纯按键操作**：K1/K2 单击切换焦点、双击确认、长按开关菜单；
- 波形页固定 5×10 网格，自动量程按 1-2-5 标准格值锚定，刻度去尾零（3V / 0.2A 而非 3.00V / 0.200A）；
- 弹窗宽度随屏幕方向自适应（横屏 260px / 竖屏 216px），竖屏不再被裁切。

### 4.4 网页控制台与 API

- 单页内嵌在固件中（不依赖外部 CDN），断网可用；
- WebSocket `/ws` 以 15Hz 推送快照，断线自动退化为 100ms 轮询并每 5s 重连；
- 五个页签：实时数据 / 波形图 / 设置 / API / 日志；设置项打开即回读设备当前值；
- 主要接口：`/api/state`、`/api/config`（GET/POST）、`/api/info`、`/api/wifi/scan`、
  `/api/export.csv`、`/ws`；
- API 页内置「▶ Test」，可就地发请求并显示状态码、耗时与响应正文。

### 4.5 工程化

- **双 OTA 分区 + 回滚**：`ota_update_start(url)` 拉取式升级写入备用分区，启动失败自动回退旧版本
  （网页控制台暂未提供固件上传入口，救回仍以 USB/串口烧录为准）；
- **coredump**：崩溃现场写入独立分区，重启后可导出分析；
- **PC 端 UI 模拟器**：`tools/ui_sim` 用同一份界面源码在 Windows 上渲染并批量截图，
  烧录前做布局回归；
- **字体子集化**：`tools/fonts/make_fonts.py` 扫描全部界面字符串生成 GB2312 一级+二级字库。



---



## 五、外壳与组装

![装配体爆炸图](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-11-07_01-15-56_5a38d0b5a629dd5d8636f259a4468106.webp)

![外壳零件](https://tc2.zeruns.com/Pic/2026/11/IMG_0897_759e97481326c01fa30d892ebc148365.webp)

![配件](https://tc2.zeruns.com/Pic/2026/11/IMG_0896_9e810b8cf1a0420ec963f3d021957efb.webp)

外壳使用 SolidWorks 设计，源文件在 `04-外壳设计/`，3MF 打印文件在 `05-生产文件/03-外壳3D打印文件/`。
顶盖、中框、底盖 三件式结构。

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261021_231338-1794078515983-23_754f05b2abf53e373bfc09801c68e31f.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261021_225920_64f47a4f724b664714c0207b6313201f.webp)

组装顺序：

1. 中框上的8个螺丝孔装入热熔螺母（可使用电烙铁加热压入）；
2. 下板装入底盖，放上中框，中框方向要留意如图所示，然后拧上底盖的四颗螺丝；
3. 电池包上青稞纸后将电池连接线插到上板的电池座上，用双面胶将电池粘在上板的底面（ESP32模组所在的面）；
4. 将上板与电池盖到中框上，注意排针连接；
5. 把上盖盖上；
6. 最后接上屏幕，屏幕背面贴双面胶固定



---



## 六、制作与复现

### 6.1 物料与成本

- 完整 BOM：`05-生产文件/02-贴片/01-上板_V2-BOM.xlsx`、`01-下板-BOM.xlsx`；
- 交互式焊接辅助：同目录 `04-上板_V2-交互式BOM-焊接辅助.html`（浏览器打开，点选位号高亮）；
- 位号图：`03-上板_V2-位号图.pdf` / `03-下板-位号图.pdf`。

下面是主要物料的成本估算表（价格随市场波动，仅供参考）：

| 物料 | 数量 | 参考单价（元） | 备注 |
| :-: | :-: | :-: | :-: |
| ESP32-S3-WROOM-1-N16R8 | 1 | 31.6 | 主控&WiFi模组 |
| ADS131M02 | 1 | 16.8 | 24bit ADC |
| 1.83寸电容触摸LCD屏，NV3030B | 1 | 14.8 | 含排线 |
| 1mΩ / 5W ，四端开尔文分流电阻 | 1 | 3.69 | 四端开尔文 |
| 500mAh 锂电池 | 1 | 8.2 | 单节 |
| PCB（上板 + 下板） | 2 |  | 嘉立创可免费打样 |
| 外壳（3D 打印） | 1 套 | 15 | 上盖/中框/底盖 |

### 6.2 打板与钢网

- 上板 Gerber：`05-生产文件/01-PCB/Gerber_上板_V2.1.zip`；
- 下板 Gerber：`05-生产文件/01-PCB/Gerber_下板_2026-09-22.zip`；
- 钢网：`05-生产文件/01-PCB/手工焊接用钢网/上板V2钢网.GTS`（手工焊接用，顶层和底层都在一片上，淘宝上下单激光钢网就行）。

> 下单时板厚建议 1.2mm，阻抗/层压按默认工艺即可；

### 6.3 焊接

1. 买锡膏，钢网对位固定好一边后刷锡膏上去
2. 刷完锡膏就放贴片器件
3. 加热台或热风枪加热焊接
4. 焊直插器件

### 6.4 烧录

Windows 一键烧录（需先装 ESP-IDF v6.1，或使用已配置好的环境）：

```powershell
cd 02-Software/XT60-Power-Meter-Firmware
.\build_fw.bat			# 编译固件
.\flash_fw.bat COM3     # 烧录固件，COM口号以设备管理器为准
```

也可以直接用浏览器 **ESPConnect** 烧录，无需安装 ESP-IDF。
镜像偏移表、三种烧录方式与排错见
[构建与烧录指南](02-Software/XT60-Power-Meter-Firmware/docs/build_and_flash.md)。

### 6.5 校准

1. 空载状态下进入设置 → 测量设置 → 电流零点校准，采样 3 秒自动归零；
2. 电压校准：输入已知标准电压（如 12.000V），按实测值修正增益/零偏；
3. 电流校准：串联标准表或电子负载，按实测值修正；
4. 校准参数写入 NVS，掉电与升级均保留。

---

## 七、使用说明

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_173809_56f7d3083d82ebe94c2d913f47a490ef.webp)

1. **接线**：XT60 输入端接电源/电池，输出端接负载，注意**正负极**；输入输出方向可以调转，支持负电流显示；
   额定 ±30A，短时峰值可测 ±75A（额定电流受 XT60 接口限制，长期大电流请自行换 XT90 底板）；
2. **开机**：拨动开关切换到USB/电池供电即可开机，如果拨动开关在XT60供电模式则通上5V以上电压也可开机，启动页显示初始化步骤进度（含硬件自检；启动页文案用编进固件的极小子集，首帧即完整中文；主界面全量字库仍后台加载），完成后进入测量主界面；
3. **测量**：主界面显示电压、电流、功率、电能；**点击底部电能行可在 Wh / Ah 间切换**；
4. **波形**：滑动到波形页，上滑打开快捷菜单可切换时间窗与 V/I/P 模式；
5. **快速菜单**：在测量页上滑（或长按 K2）打开，可切换数据刷新率、清除电能统计；
6. **配网**：设置 → WiFi 与热点 → 搜索WiFi并输入密码（也可使用热点模式，手机或电脑连接设备热点进入网页控制台进行配网）；连接成功后状态栏显示 WiFi 图标；
7. **网页控制台**：浏览器访问设备 IP，实时数据、波形、设置、API、日志五个页签；
8. **充电**：USB-C 接入 5V 电源，TP4057 以 400mA 充电，屏幕状态栏显示电量。

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261025_011253_b4e3d07804e40ab81382d8fc79149b08.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261025_011344_6d9247c8228a32215e16554c4a73f171.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261025_011412_e02af3aa571a7e24d94e0caee8d4fb5e.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261025_011421_7e25a38a98eb3925049007300bf238fa.webp)

> 空载时电流显示 0 是**正常的**：|I| < 2mA 时固件按 0 处理（小信号死区），
> 消除 1mΩ 分流器固有噪声造成的末位跳动；波形页仍显示原始采样，可观察 mA 级变化。



功率计自身功耗大概在0.7瓦左右。

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261025_011603_d3efead4ab87642a4cdedd20f82ab0e5.webp)



---



## 八、待实现功能

1. OTA升级功能
2. USB串口上位机功能及配套上位机软件
3. ...

这些功能我有空会做，你们也可以自己做了后提PR。



---



## 九、目前存在的问题

① 使用电池/USB供电模式下时3.3V供电电压会通过XT60供电模式下的buck电路内部MOS的体二极管与供电控制的MOS管（Q4）的体二极管倒灌到XT60接口，所以没有输入的情况下也会显示有个2.45V左右的电压（3.3V经MOS体二极管压降）。

> 此问题的解决方式有：
> 1. 在Q4后面多加一个MOS背对着
> 1. 如果不想改电路的，也用不到XT60取电供电的，可以直接不焊XT60取电供电这部分buck电路，只用电池供电
> 1. 无需电池/USB供电的，直接把开关切换到XT60供电模式

![](https://tc2.zeruns.com/Pic/2026/11/image-20261109013933541_f7bf345408e05552d5e0e115956b14c5.png)




---



## 十、仓库结构


```
XT60-Power-Meter/
├── 01-Hardware/              硬件设计
│   ├── 01-XT60功率计-立创EDA工程.epro2
│   ├── 02-上板原理图_V1.pdf / V2.pdf
│   └── 02-下板原理图.pdf
├── 02-Software/
│   └── XT60-Power-Meter-Firmware/    ESP-IDF 固件工程
│       ├── components/               驱动 / 中间件 / 服务
│       ├── docs/                     架构、烧录、引脚表
│       └── tools/                    字体工具、LVGL UI 模拟器
├── 03-Datasheet/             全部器件数据手册（中/英文）
├── 04-外壳设计/              SolidWorks 源文件（上盖/中框/底盖/装配体）
├── 05-生产文件/
│   ├── 01-PCB/               Gerber、钢网
│   ├── 02-贴片/              BOM、位号图、交互式焊接辅助
│   └── 03-外壳3D打印文件/    3MF 文件
├── images/                   README 图片
├── LICENSE                   硬件许可证（CERN OHL-S v2）
└── README.md
```



---



## 十一、文档索引

| 文档 | 内容 |
| --- | --- |
| [固件 README](02-Software/XT60-Power-Meter-Firmware/README.md) | 固件功能、硬件概要、构建与烧录 |
| [构建与烧录指南](02-Software/XT60-Power-Meter-Firmware/docs/build_and_flash.md) | 5 个镜像、三种烧法、判断表与排错 |
| [架构设计](02-Software/XT60-Power-Meter-Firmware/docs/architecture.md) | 分层结构、双核分工、测量链路、踩坑记录 |
| [引脚分配表](02-Software/XT60-Power-Meter-Firmware/docs/hardware_pinout.md) | 全部 GPIO 与外设映射 |
| [贡献指南](02-Software/XT60-Power-Meter-Firmware/CONTRIBUTING.md) | 提交规范、代码风格、注释要求 |
| [第三方许可](02-Software/XT60-Power-Meter-Firmware/THIRD_PARTY.md) | LVGL / esp_lvgl_port / 字体等许可 |



---



## 十二、参与贡献

1. Fork 本仓库，新建 `feat/xxx` 或 `fix/xxx` 分支；
2. 提交信息遵循 Conventional Commits，使用**中文描述**；
3. 固件改动请先跑 `tools/ui_sim` 模拟器场景回归，再上实机验证；
4. 提交 Pull Request 并说明改动动机、验证方式与影响范围。

硬件改动（原理图/PCB/外壳）请附改动说明与实物照片；发现测量异常请提供串口日志与
`/api/info` 版本信息。



---



## 十三、许可证

- **硬件设计**（原理图、PCB、外壳、生产文件）：[CERN OHL-S v2](LICENSE)
  —— 强互惠开源硬件许可证，基于本设计的衍生硬件须以相同许可证开源；
- **固件源码**（`02-Software/`）：[MIT](02-Software/XT60-Power-Meter-Firmware/LICENSE)；
- 第三方组件（LVGL、esp_lvgl_port、Noto Sans SC 等）保留各自许可，
  详见 [THIRD_PARTY.md](02-Software/XT60-Power-Meter-Firmware/THIRD_PARTY.md)。



---



## 十四、免责声明

- 本设备测量结果**不作为计量认证依据**；
- **90V / ±75A 峰值（额定 ±30A）** 属于危险电压电流，接线、焊接与测试时请注意防短路、防触电、防反接，
  大电流测试请使用合适的线径与接头；
- 使用本项目硬件或固件造成的任何直接或间接损失，作者不承担责任；
- 欢迎通过 Issue 反馈问题与改进建议。



---



## 推荐阅读

- **高性价比和便宜的VPS/云服务器推荐:** [https://blog.zeruns.com/archives/383.html](https://blog.zeruns.com/archives/383.html)
- 我的世界开服教程：[https://blog.zeruns.com/tag/mc/](https://blog.zeruns.com/tag/mc/)
- 【开源】基于SC8703的升降压DCDC可调电源（Buck-Boost），电压电流可调，支持PD快充输入：[https://blog.zeruns.com/archives/929.html](https://blog.zeruns.com/archives/929.html)
- 【开源】24V3A反激式开关电源（基于UC3842，含电路和变压器参数计算过程）：[https://blog.zeruns.com/archives/910.html](https://blog.zeruns.com/archives/910.html)
- **基于STM32的同步整流Buck-Boost数字电源 开源：** [https://blog.zeruns.com/archives/791.html](https://blog.zeruns.com/archives/791.html)
- 开源 140W+65W升降压PD3.1快充模块(2C+1A口)，IP6557+IP6538，205W桌面充电器：[https://blog.zeruns.com/archives/801.html](https://blog.zeruns.com/archives/801.html)
- **开源TypeC拓展坞，4个10Gbps的USBA口+2.5G网卡+读卡器：**[https://blog.zeruns.com/archives/868.html](https://blog.zeruns.com/archives/868.html)

