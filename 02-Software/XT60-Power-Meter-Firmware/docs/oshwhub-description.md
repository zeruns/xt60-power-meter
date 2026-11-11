# 立创开源平台（oshwhub.com）发布文案

> 用法：第一部分填「项目标题」，第二部分填「项目简介」，第三部分整体复制到「项目描述」编辑框（Markdown 模式）。
> 图片采用项目根 README 的 `tc2.zeruns.com` 直链，已抽验可访问；若立创编辑器外链缓存失败，请重新上传对应图片。
> 内容依据项目根 README、固件 README 与架构文档整理；量程、功耗、待实现功能和已知问题不另行推测。

---

## 一、开源标题（4~32 字符）

**XT60 功率计：90V/75A 24bit 触控彩屏 WiFi**（32 字符）

备选：

1. 90V/75A XT60 功率计：24bit 触控彩屏（27 字符）
2. XT60 功率计：24bit + 触控彩屏 + WiFi（28 字符）

## 二、项目简介（83 字符，≤100）

90V 电压、75A 峰值电流、24bit 同步采样的开源 XT60 功率计：触控彩屏 + 示波器波形，WiFi 网页控制台，电能容量积分与越限告警，软硬件全开源。

## 三、项目描述（以下内容整体复制到描述框）

<div style="font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','Microsoft YaHei',sans-serif;background:#f3f6fa;padding:clamp(20px,4vw,48px);border-radius:32px;overflow:hidden;box-sizing:border-box">
<div style="display:flex;flex-wrap:wrap;gap:34px;align-items:center;max-width:1240px;margin:0 auto;box-sizing:border-box">
<div style="flex:1;min-width:min(100%,300px)">
<div style="display:inline-block;padding:7px 12px;background:#e1edff;color:#2559a7;border-radius:50px;font:700 10px/1.5 Consolas,'Microsoft YaHei',monospace;letter-spacing:1px">XT60 POWER METER / OPEN HARDWARE</div>
<div style="font-size:clamp(46px,7vw,88px);font-weight:900;letter-spacing:-4px;line-height:1.08;color:#10263e;margin:22px 0 16px">XT60 功率计</div>
<div style="font-size:clamp(23px,3vw,38px);font-weight:750;line-height:1.35;color:#2768c7">90V / 75A 峰值<br>24bit · 触控彩屏 · WiFi</div>
<div style="font-size:14px;line-height:1.9;color:#62758b;margin-top:18px">XT60 直插测量 · 示波器风格波形 · 网页控制台<br>硬件、固件、外壳与生产文件全开源</div>
<div style="display:flex;flex-wrap:wrap;gap:7px;margin-top:22px">
<div style="padding:7px 11px;background:#18324d;color:#fff;border-radius:50px;font:700 10px/1.5 Consolas,monospace">ESP32-S3</div>
<div style="padding:7px 11px;background:#dff3e9;color:#246b50;border-radius:50px;font:700 10px/1.5 Consolas,monospace">ADS131M02</div>
<div style="padding:7px 11px;background:#e8e3f8;color:#71569c;border-radius:50px;font:700 10px/1.5 Consolas,monospace">4 kSPS</div>
<div style="padding:7px 11px;background:#fff0d9;color:#9b5d15;border-radius:50px;font:700 10px/1.5 Consolas,monospace">LVGL 9</div>
</div>
</div>
<div style="flex:1.15;min-width:min(100%,300px)">
<div style="line-height:0;overflow:hidden;border-radius:28px;border:7px solid #ffffff;box-shadow:0 24px 60px #334f731f;transform:rotate(-2deg);background:#e8eef5">
<img src="https://tc2.zeruns.com/Pic/2026/10/IMG_20261031_163238%281%29_f6cebf8d83645903a56acc390aabcf88.webp" width="100%" alt="XT60 功率计多种外壳成品，彩色屏幕分别显示测量、波形、设置与启动页">
</div>
</div>
</div>
<div style="display:flex;flex-wrap:wrap;max-width:1240px;margin:30px auto 0;border-top:1px solid #dce4ed;border-bottom:1px solid #dce4ed">
<div style="flex:1;min-width:125px;padding:20px 14px;box-sizing:border-box"><div style="font-size:30px;font-weight:850;color:#142b45">0~90V</div><div style="font-size:11px;color:#738398;margin-top:6px">设计直流电压量程</div></div>
<div style="flex:1;min-width:125px;padding:20px 14px;box-sizing:border-box"><div style="font-size:30px;font-weight:850;color:#142b45">±75A</div><div style="font-size:11px;color:#738398;margin-top:6px">峰值电流 · 额定 ±30A</div></div>
<div style="flex:1;min-width:125px;padding:20px 14px;box-sizing:border-box"><div style="font-size:30px;font-weight:850;color:#142b45">24bit</div><div style="font-size:11px;color:#738398;margin-top:6px">双通道同步采样 ADC</div></div>
<div style="flex:1;min-width:125px;padding:20px 14px;box-sizing:border-box"><div style="font-size:30px;font-weight:850;color:#142b45">1.83 寸</div><div style="font-size:11px;color:#738398;margin-top:6px">IPS 电容触控彩屏</div></div>
</div>
</div>

## 01 / 项目概述

做航模、无人机、电池充放电或电源调试时，经常需要一台能**直接插进 XT60 回路**的功率计：既要看清大电流下的电压跌落，也要回看插拔瞬间的波形，最好还能把数据同步到电脑。

XT60 功率计以 **ESP32-S3** 为主控，采用 **TI ADS131M02 24bit 双通道同步采样 ADC（4 kSPS）**，配合 1mΩ 四端开尔文分流器与 101:1 精密分压，测量 **0~90V、额定 ±30A（峰值 ±75A）** 的直流回路。

本地是一块 1.83 寸 IPS 触控彩屏，运行 LVGL 9 界面；连接 WiFi 后，手机或电脑浏览器可以打开网页控制台，实时查看数据与波形、修改设置、导出 CSV，也可以通过 HTTP API 接入自己的上位机或自动化脚本。

<div style="margin:20px 0;padding:18px 22px;background:#fff8e9;border-left:5px solid #d98a24;border-radius:10px;color:#65491f;font-size:14px;line-height:1.9;box-sizing:border-box">
<b>开发说明：本项目软件部分由 AI 开发。</b><br>
仓库同时公开完整源码、架构文档、PC 端 LVGL 模拟器与主机端测试工具，便于阅读、修改和二次开发。用于实际测量前，请自行完成代码审查、参数校准与实机验证。
</div>

### 核心功能

| 分类 | 已实现功能 |
| :--- | :--- |
| 测量 | 电压 / 电流块均值、瞬时与平均功率、电能 Wh、容量 Ah、运行时间与峰谷值 |
| 采样 | 24bit 双通道同步采样，4 kSPS，CRC 帧校验剔除坏帧，200ms 块统计 |
| 波形 | V/I/P 多模式、双轴曲线、1s~10min 六档时间窗、1-2-5 自动量程、min/max 包络 |
| 交互 | 电容触屏 + 双按键，三页滑动、快速菜单、重力自动转屏、无操作自动降亮 |
| 告警 | 过压 / 过流 / 过功率阈值可设；双温度源过温保护恒开，任一达到 65℃ 即告警 |
| 联网 | WiFi STA/AP 配网，网页五页签，15Hz WebSocket，断线轮询与自动重连 |
| 电源 | XT60 / USB-C / 单节锂电池三路供电，USB 与电池自动切换，TP4057 充电 |
| 工程化 | 双 OTA 分区与回滚、coredump、PC 端 UI 模拟器、主机端单元测试 |

### 主要参数

| 项目 | 参数 |
| :--- | :--- |
| 主控 | ESP32-S3-WROOM-1-N16R8，16MB Flash + 8MB 八线 PSRAM |
| 计量 ADC | TI ADS131M02，24bit Σ-Δ，双通道同步采样 4 kSPS |
| 电压量程 | 设计测量 0~90V；101:1 分压理论满量程 ±121V，受辅助电源 100V 输入上限约束 |
| 电流量程 | 峰值 ±75A（ADC 满量程）；额定 ±30A（XT60 接口限制） |
| 显示触摸 | 1.83 寸 IPS，240×284；NV3030B + CST816D，QSPI + I2C |
| 传感器 | AHT20 温湿度、SC7A20HTR 三轴加速度计 |
| 通信 | 2.4GHz WiFi，STA/AP，HTTP + WebSocket |
| 供电 | XT60 / USB-C / 锂电池；TP4057 以 400mA 充电 |
| 自身功耗 | 约 0.7W |
| 外壳 | 上盖 / 中框 / 底盖三件式 3D 打印结构，SolidWorks 源文件与 3MF 已开源 |
| 当前固件 | v0.6.9，ESP-IDF v6.1 + LVGL 9.6 |

> 本机用于爱好者测量与调试，**不作为计量器具**。电流绝对值小于 2mA 时按 0 显示，以消除分流器噪声导致的空载跳动；波形页仍保留原始样本，可观察 mA 级变化。

## 02 / 成品与界面展示

### 2.1 外观

| 整机正反面 | 亮屏测量状态 |
| :---: | :---: |
| ![功率计外观正反面](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_040258_3d78c7efd72f1373efcd3bacb8796a8f.webp) | ![功率计亮屏外观](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_040311_711ff4b0826e90d076c9959e9fb6f459.webp) |

![XT60 功率计成品与接线状态](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_040708_33f38b9f22f6c5d8b28026537e9d76ab.webp)

机身采用上盖、中框、底盖三件式 3D 打印外壳，螺丝固定。正面为 1.83 寸 IPS 屏，屏幕左侧是状态指示灯与复位按键；K1/K2 实体按键可完成全部主要操作。Type-C 口旁的开关用于选择「电池 / USB」或「XT60」供电路径。

### 2.2 本地屏幕界面

| 测量主界面 | 示波器风格波形页 |
| :---: | :---: |
| ![测量页](https://tc2.zeruns.com/Pic/2026/11/ui_measure_8ab2c86641b538251224d69956f2779e.png) | ![波形页](https://tc2.zeruns.com/Pic/2026/11/ui_wave_1a829869dd821102645d119c8c5ea365.png) |
| 电压 / 电流 / 功率大数字，底部电能、温湿度与峰谷值 | V/I/P 多模式，六档时间窗，自动量程 |

| 设置页 | 上拉快速菜单 |
| :---: | :---: |
| ![设置页](https://tc2.zeruns.com/Pic/2026/11/ui_settings_33b8b3b590df65d29e27988189f20f94.png) | ![快速菜单](https://tc2.zeruns.com/Pic/2026/11/ui_quick_menu_a5f4a9ac0d1ef87bb35d6ec92ffd0f88.png) |
| 测量、显示、声音、WiFi、告警、电能统计等设置 | 数据刷新率、清除电能统计等常用操作 |

> 以上界面图由项目自带的 PC 端 LVGL 模拟器生成，与设备使用同一份界面源码。

### 2.3 网页控制台

实时数据页按信息层级展示功率主读数、电压 / 电流 / 功率、累计量、电池与 WiFi 状态、温湿度和告警状态。

![网页控制台实时数据页](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-10-25_01-59-40_84cf88d80e3008780c243f9abd56fd26.webp)

波形页支持 V/I/P 三通道叠加、时间窗切换、暂停、拖动回看、十字光标、量程锁定和 CSV 导出。

![网页控制台波形页](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-10-25_01-58-54_d3a2d4055abff141ace6b126fe2e7eaf.webp)

网页直接内嵌在固件中，不依赖外部 CDN。手机或电脑浏览器访问设备 IP 即可使用，无需安装 App；网页还包含设置、API 与日志页签，API 页可就地发送测试请求并查看状态码、耗时和响应正文。

## 03 / 硬件设计

### 3.1 硬件系统架构

![XT60 功率计硬件系统框架图](https://tc2.zeruns.com/Pic/2026/11/block-diagram_4e1dbc1b78441e2d29679fb0b7d7bc6f.webp)

整机由**上下两块 PCB**组成：上板负责主控、显示、传感器、按键、电压分压与电源管理；下板负责 XT60 输入输出和大电流分流器。电池位于上板背面、夹在两层 PCB 之间。

| 模块 | 方案与职责 |
| :--- | :--- |
| 主控 | ESP32-S3-WROOM-1-N16R8：采样调度、UI、WiFi、存储 |
| 计量前端 | ADS131M02 + LRA8551A：24bit 同步采样与电压通道缓冲 |
| 显示交互 | NV3030B LCD、CST816D 触摸、K1/K2、蜂鸣器、状态灯 |
| 传感器 | AHT20 温湿度、SC7A20HTR 加速度计 |
| 电源 | SY8511BADC、TLV62084、TPS2116、TP4057 |
| 存储升级 | 16MB Flash、独立字体 storage、双 OTA 分区 |

### 3.2 电压与电流采样

<div style="display:flex;flex-wrap:wrap;gap:14px;margin:18px 0">
<div style="flex:1;min-width:min(100%,320px);padding:20px;background:#f4f8ff;border-top:4px solid #3976c8;border-radius:10px;box-sizing:border-box">
<b>电压通道 · 101:1 精密分压</b>
<div style="font-size:13px;line-height:1.9;color:#53677e;margin-top:8px">100kΩ / 1kΩ、±0.1% 薄膜电阻分压，经零漂移运放 LRA8551A 缓冲后进入 ADS131M02 CH1（PGA 1x）。按设计量程折算约 14.5µV/LSB；辅助电源输入上限将整机设计测量范围约束为 0~90V。</div>
</div>
<div style="flex:1;min-width:min(100%,320px);padding:20px;background:#fff7ed;border-top:4px solid #d88027;border-radius:10px;box-sizing:border-box">
<b>电流通道 · 四端开尔文分流</b>
<div style="font-size:13px;line-height:1.9;color:#6f5b43;margin-top:8px">1mΩ / 5W 分流器，采样点独立于大电流走线，RC 滤波后进入 ADS131M02 CH0（PGA 16x）。ADC 满量程对应 ±75A，约 8.94µA/LSB；分流器本身允许最大 70A，XT60 接口将持续额定电流限制为 ±30A。</div>
</div>
</div>

- 70A 时分流器压降约 70mV、功耗约 4.9W；30A 时压降约 30mV、功耗约 0.9W。
- 小电流噪声的代价由 200ms 块均值和 2mA 空载死区处理，原始波形不被死区覆盖。
- 需要长期测量更大电流时，可自行修改底板并改用 XT90 接口。

### 3.3 电源与电池管理

- **XT60 供电**：SY8511BADC 降压到 3.3V / 1A。
- **USB / 电池供电**：TLV62084 降压到 3.3V / 2A；V2 使用 TPS2116 在 USB 与电池之间切换，优先 USB。
- **充电**：TP4057 以 400mA 为单节锂电池充电；电池电压经分压进入 ESP32-S3 ADC，用于电量显示。
- V1 另有分立 MOS 实现电源切换的方案，原理图一并开源。

### 3.4 PCB 实物与设计要点

| 上板正反面与不同版本 | 下板功率路径与 XT60 接口 |
| :---: | :---: |
| ![上板 PCB 实物](https://tc2.zeruns.com/Pic/2026/11/IMG_0872_22d390e02a0eba572fcd520400ed8f82.webp) | ![下板 PCB 实物](https://tc2.zeruns.com/Pic/2026/11/IMG_0903_046bb89dd6296045676408ae1e18d249.webp) |

设计重点：

1. 上下板叠层，通过排针 / 排母连接；大电流路径全部放在下板。
2. ISENSE 差分小信号走线尽量短，远离 DC-DC 电感和开关节点。
3. ADC 的 SPI 与 DRDY 走线缩短，避免与高频时钟和开关噪声长距离并行。
4. 大电流铜箔加粗并配合铜排，降低压降与温升；完整地 / 电源处理见立创 EDA 工程。

## 04 / 外壳、爆炸图与内部结构

### 4.1 装配体爆炸图

![SolidWorks 装配体爆炸图](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-11-07_01-15-56_5a38d0b5a629dd5d8636f259a4468106.webp)

外壳使用 SolidWorks 设计，包含上盖、中框、底盖与装配体源文件；3MF 打印文件位于生产资料目录。中框通过热熔螺母提供上下盖的固定螺纹。

| 外壳零件 | 螺丝、热熔螺母、电池与屏幕等配件 |
| :---: | :---: |
| ![外壳零件](https://tc2.zeruns.com/Pic/2026/11/IMG_0897_759e97481326c01fa30d892ebc148365.webp) | ![组装配件](https://tc2.zeruns.com/Pic/2026/11/IMG_0896_9e810b8cf1a0420ec963f3d021957efb.webp) |

### 4.2 内部结构

| 下板功率板与上板主控板 | 上下板叠层与电池布置 |
| :---: | :---: |
| ![内部上下板结构](https://tc2.zeruns.com/Pic/2026/11/IMG_20261021_225034_b2862a188c01ba6d2c8e752f381a37b5.webp) | ![内部结构细节](https://tc2.zeruns.com/Pic/2026/11/IMG_20261021_231338_754f05b2abf53e373bfc09801c68e31f.webp) |

电池固定在上板背面，即 ESP32-S3 模组所在的一面，并夹在上下板之间。建议使用双面胶或泡棉固定，并包覆青稞纸做好绝缘。

### 4.3 组装顺序

1. 在中框的 8 个螺丝孔中压入热熔螺母，可使用电烙铁加热压入。
2. 下板装入底盖，按图示方向放上中框，拧入底盖四颗螺丝。
3. 电池包覆青稞纸，接上电池座，用双面胶固定在上板底面。
4. 对准板间排针，将上板与电池组合装入中框。
5. 装上上盖并固定。
6. 最后连接屏幕，屏幕背面用双面胶固定。

## 05 / 软件架构说明

固件基于 **ESP-IDF v6.1 + LVGL 9.6**，分层依赖只能自上而下，业务代码不直接操作裸外设。

```text
main/                  启动流程、任务创建与参数下发
components/
├── drivers/           ADS131M02、LCD、触摸、按键、蜂鸣器、IMU 等驱动
├── middleware/        data_store、web_console、wifi_manager、lvgl_port、fs_storage
└── services/          power_measure、ui、settings、orientation、system_state
tools/
├── ui_sim/            PC 端 LVGL UI 模拟器与场景截图回归
└── fonts/             主界面字库与启动页内嵌子集生成工具
```

### 测量链路

1. ADS131M02 每 250µs 产生一次 DRDY，SPI DMA 读取完整 4 字帧：STATUS / CH0 / CH1 / CRC。
2. CRC-16/CCITT 校验通过后，按标定参数换算电压、电流与瞬时功率。
3. 原始样本写入 PSRAM 三级环形缓冲，供波形页回看。
4. 每 200ms 结算块均值、平均功率、峰谷值与告警，并积分电能和容量。
5. 每 25ms 产出 40Hz 显示快照，UI 与网页均只读快照，避免并发直接修改测量数据。

**双核分工**：Core 1 负责采样与测量任务，Core 0 负责 LVGL 渲染、按键交互和 WiFi / HTTP 服务；低优先级周期任务按架构设计分配到 Core 1，减少交互核压力。

### 工程化设计

- **启动与字体**：启动页显示 18 步初始化 + 5 项硬件自检的真实进度；启动文案使用内嵌极小子集，主界面全量中文字库后台加载。
- **OTA 基础**：双 OTA 分区、拉取式升级中间件与启动回滚校验；网页端暂未提供固件上传入口。
- **故障取证**：独立 coredump 分区保存崩溃现场，便于后续导出分析。
- **开发验证**：PC 端 UI 模拟器复用真机界面源码；主机端纯逻辑单元测试用于不依赖硬件的验证。

## 06 / 开源资料下载与目录结构

<div style="margin:20px 0;padding:22px 24px;background:#17324d;border-radius:16px;color:#dce9f7;box-sizing:border-box">
<div style="font-size:20px;font-weight:800;color:#ffffff">复刻请下载完整仓库，而不只是固件目录</div>
<div style="font-size:13px;line-height:1.9;margin:8px 0 16px">原理图、立创 EDA 工程、Gerber、BOM、钢网、SolidWorks 外壳与 3MF 打印文件都在同一个 Gitee 仓库中。</div>
<a href="https://gitee.com/zeruns/xt60-power-meter" style="display:inline-block;padding:10px 18px;background:#79b7ff;color:#102a44;text-decoration:none;border-radius:8px;font-weight:800">前往 Gitee 下载软硬件资料（develop 分支）</a>
</div>

```text
XT60-Power-Meter/
├── 01-Hardware/                     硬件设计
│   ├── 01-XT60功率计-立创EDA工程.epro2
│   ├── 02-上板原理图_V1.pdf / V2.pdf
│   └── 02-下板原理图.pdf
├── 02-Software/
│   └── XT60-Power-Meter-Firmware/   ESP-IDF 固件工程
│       ├── main/                    启动与任务编排
│       ├── components/              驱动 / 中间件 / 服务
│       ├── docs/                    架构、烧录、引脚文档
│       └── tools/                   字体工具、UI 模拟器等
├── 03-Datasheet/                    器件数据手册与屏幕资料
├── 04-外壳设计/                     SolidWorks 源文件与 PCB 模型
│   ├── 上盖.SLDPRT / 中框.SLDPRT / 底盖.SLDPRT
│   ├── 外壳装配体.SLDASM
│   └── PCB模型.zip
├── 05-生产文件/
│   ├── 01-PCB/                      Gerber 与手工焊接钢网
│   ├── 02-贴片/                     BOM、位号图、交互式焊接辅助
│   └── 03-外壳3D打印文件/           3MF 打印文件
├── LICENSE                          硬件 CERN OHL-S v2
├── README.md                        中文项目说明
└── README.en.md                     英文项目说明
```

也可用 Git 获取完整资料：

```powershell
git clone -b develop https://gitee.com/zeruns/xt60-power-meter.git
```

## 07 / 制作与复刻说明

### 7.1 主要物料

完整 BOM、位号图与交互式焊接辅助位于 `05-生产文件/02-贴片/`。以下价格来自项目 README，仅作参考，会随采购时间和渠道变化。

| 物料 | 数量 | 参考单价（元） | 备注 |
| :--- | :---: | ---: | :--- |
| ESP32-S3-WROOM-1-N16R8 | 1 | 31.6 | 主控与 WiFi 模组 |
| ADS131M02 | 1 | 16.8 | 24bit ADC |
| 1.83 寸 NV3030B 电容触摸 LCD | 1 | 14.8 | 含排线 |
| 1mΩ / 5W 四端开尔文分流器 | 1 | 3.69 | 电流采样 |
| 500mAh 单节锂电池 | 1 | 8.2 | 电池 |
| 3D 打印外壳 | 1 套 | 15 | 上盖 / 中框 / 底盖 |

### 7.2 复刻步骤

1. **下载资料**：从 Gitee 获取完整仓库，核对原理图版本与生产文件版本。
2. **打板与钢网**：Gerber 位于 `05-生产文件/01-PCB/`；项目建议板厚 1.2mm，阻抗与层压按默认工艺。
3. **采购与焊接**：按 BOM 备料；刷锡膏 → 贴装 → 加热台或热风枪焊接 → 焊直插器件。
4. **打印外壳**：使用 `05-生产文件/03-外壳3D打印文件/` 中的 3MF，并准备热熔螺母、螺丝与绝缘材料。
5. **烧录固件**：使用 ESP-IDF 构建烧录，或使用浏览器 ESPConnect。
6. **组装与校准**：按爆炸图和组装顺序装机，再进行电流零点与电压 / 电流增益校准。

### 7.3 构建与烧录

```powershell
cd xt60-power-meter/02-Software/XT60-Power-Meter-Firmware
.\build_fw.bat          # 构建固件
.\flash_fw.bat COM3     # 烧录；COM 口以设备管理器为准
```

也可以不安装 ESP-IDF，使用 Chrome / Edge 打开 **ESPConnect**，通过 Web Serial 烧录完整固件。5 个镜像的组成、偏移、烧录方式与排错见仓库 `docs/build_and_flash.md`。

### 7.4 校准

1. 空载进入「设置 → 测量设置 → 电流零点校准」，设备采样 3 秒块均值并自动归零。
2. 输入已知标准电压，按参考仪表读数修正电压增益 / 零偏。
3. 串联标准表或电子负载，按实测值修正电流增益 / 零偏。
4. 校准参数写入 NVS，掉电与固件升级后保留。

## 08 / 使用说明

![功率计接入测试回路](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_173809_56f7d3083d82ebe94c2d913f47a490ef.webp)

1. **接线**：XT60 输入端接电源 / 电池，输出端接负载，注意正负极；输入输出方向可调换，支持负电流显示。
2. **开机**：开关拨到 USB / 电池供电即可启动；若选择 XT60 供电，输入需达到 5V 以上。
3. **测量**：主界面显示电压、电流、功率和电能；点击底部电能行可在 Wh / Ah 之间切换。
4. **波形**：滑动到波形页，上滑打开快捷菜单，切换时间窗与 V/I/P 模式。
5. **快速菜单**：测量页上滑或长按 K2，可调整数据刷新率或清除电能统计，清除前有二次确认。
6. **配网**：在「设置 → WiFi 与热点」搜索 WiFi 并输入密码；也可连接设备热点后通过网页配网。
7. **网页控制台**：连接成功后，在同一局域网的浏览器中访问设备 IP。
8. **充电**：USB-C 接入 5V 电源，TP4057 以 400mA 充电，状态栏显示电池电量。

> **读数习惯**：空载电流显示 0 属于正常现象，这是 2mA 小信号死区的作用；项目 README 记录的整机自身功耗约 0.7W。

## 09 / 待实现功能与目前存在的问题

<div style="display:flex;flex-wrap:wrap;gap:14px;margin:18px 0">
<div style="flex:1;min-width:min(100%,320px);padding:20px;background:#eef5ff;border-radius:12px;box-sizing:border-box">
<b>待实现功能</b>
<div style="font-size:13px;line-height:1.95;color:#4f647c;margin-top:8px">
1. 完整的 OTA 升级使用流程与网页端升级入口。当前已有双 OTA 分区、拉取式升级中间件和启动回滚，网页控制台尚未提供固件上传入口。<br>
2. USB 串口上位机功能及配套上位机软件。<br><br>
欢迎按仓库贡献指南提交 Pull Request。
</div>
</div>
<div style="flex:1;min-width:min(100%,320px);padding:20px;background:#fff3ed;border-radius:12px;box-sizing:border-box">
<b>已知硬件问题：USB / 电池供电时的电压倒灌</b>
<div style="font-size:13px;line-height:1.95;color:#735546;margin-top:8px">
在电池 / USB 供电模式下，3.3V 会经过 XT60 供电 Buck 电路内部 MOS 的体二极管与供电控制 MOS（Q4）的体二极管倒灌到 XT60 接口。因此即使 XT60 没有接输入，也可能显示约 2.45V。
</div>
</div>
</div>

可选解决方式：

1. 在 Q4 后增加一颗背对背 MOS，从硬件上阻断倒灌。
2. 若不使用 XT60 取电，可不焊该路 Buck 供电电路，仅使用电池 / USB 供电。
3. 若不需要电池 / USB 供电，直接将开关切换到 XT60 供电模式。

![电压倒灌问题电路说明](https://tc2.zeruns.com/Pic/2026/11/image-20261109013933541_f7bf345408e05552d5e0e115956b14c5.png)

## 10 / 许可证、安全与参与贡献

### 开源许可

- **硬件设计**（原理图、PCB、外壳、生产文件）：**CERN OHL-S v2**，属于强互惠开源硬件许可，衍生硬件须按相同许可证开源。
- **固件源码**：**MIT**。
- **第三方组件**：LVGL、esp_lvgl_port、Noto Sans SC 等保留各自许可，详见固件仓库 `THIRD_PARTY.md`。

### 安全与免责声明

- 测量结果不作为计量认证依据。
- 90V / ±75A 峰值属于危险电压电流；接线、焊接和测试时请防短路、防触电、防反接，大电流测试使用合适线径与接头。
- 复刻前请核对原理图、PCB 版本、供电路径与分流器散热，并先在低压小电流条件下验证。
- 使用本项目硬件或固件造成的直接或间接损失，作者不承担责任。

<div style="margin:26px 0 8px;padding:24px;background:linear-gradient(135deg,#17324d,#28527a);border-radius:18px;color:#d8e7f7;box-sizing:border-box">
<div style="font-size:21px;font-weight:800;color:#ffffff">从一台功率计，到一套完整的开源工程</div>
<div style="font-size:13px;line-height:1.95;margin-top:10px">你可以直接复刻，也可以只取其中一部分：24bit 测量链路、LVGL 三页界面、内嵌 Web 控制台、双 PCB 结构或 SolidWorks 外壳。<br>如果项目对你有帮助，欢迎点赞、收藏、克隆工程；问题与改进建议可提交到 Gitee Issue，也欢迎 Pull Request。</div>
<div style="margin-top:14px"><a href="https://gitee.com/zeruns/xt60-power-meter" style="color:#a9d1ff;font-weight:700">Gitee：gitee.com/zeruns/xt60-power-meter</a></div>
</div>
