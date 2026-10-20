# 固件组成与烧录指南

> 面向第一次拿到本固件的人：先看懂"哪几个文件、各自管什么"，再选一种烧录方式。
> 引脚与硬件见 [hardware_pinout.md](hardware_pinout.md)，代码架构见 [architecture.md](architecture.md)。

---

## 1. 固件由 5 个文件组成

`idf.py build` 之后，`build/` 目录下会生成下面 5 个镜像。它们**不是任意排列**，
各自必须烧到分区表规定的偏移处，烧错位置设备就起不来。

| 文件 | 烧录偏移 | 大小（参考） | 作用 | 什么时候必须更新 |
|---|---|---|---|---|
| `build/bootloader/bootloader.bin` | `0x0` | ~22 KB | 二级引导程序。上电后初始化 Flash/PSRAM、读分区表、校验并跳转到应用；含 OTA 回滚判定逻辑 | 改分区表、改引导相关 sdkconfig、换 Flash/PSRAM 模式时 |
| `build/partition_table/partition-table.bin` | `0x8000` | ~3 KB | 分区表本体（定义 NVS / OTA / storage 各段在哪） | 改 `partitions.csv` 时（**必须连同 bootloader 一起**） |
| `build/ota_data_initial.bin` | `0xf000` | ~8 KB | OTA 选择数据的初始值（告诉 bootloader 首次从 `ota_0` 启动） | 首次烧录、或想强制回到出厂槽位时 |
| `build/XT60-Power-Meter-Firmware.bin` | `0x20000` | ~1.5 MB | **应用固件**（全部业务代码 + 内嵌网页控制台）。日常改功能只动它 | 每次改代码都要 |
| `build/storage.bin` | `0x830000` | 8.0 MB（镜像）| SPIFFS 文件系统镜像，里面是**两套中文字体**（`/spiffs/fonts/notosc_reg.ttf`、`notosc_bold.ttf`，约 4.2 MB 实际内容） | 改字库/改 SPIFFS 内文件时；**OTA 不会更新它** |

### 分区表长什么样（`partitions.csv`）

16 MB Flash 被切成：

```
0x000000 ─ bootloader + 分区表 + NVS/otadata/phy_init（系统区）
0x020000 ─ ota_0   4 MB   ← 当前运行的固件 A 槽
0x420000 ─ ota_1   4 MB   ← OTA 升级写入的 B 槽（A/B 双槽可回滚）
0x820000 ─ coredump 64 KB ← 崩溃现场（跑 `espcoredump` 取证）
0x830000 ─ storage 7.81 MB ← SPIFFS（中文字体）
0x1000000 = 16 MB 用满
```

**为什么字体不放进 app**：全量中文字体 4 MB 级，若塞进 4 MB 的 app 分区会直接放不下；
放独立 `storage` 分区还能让 OTA 只传 1.5 MB 的 app（升级快、流量省）。
**代价**：改字体必须**全量烧录**（OTA 只覆盖 `ota_0/ota_1`，碰不到 `storage`）——
这是本项目最容易踩的一个坑，表现为"OTA 升级成功但中文仍显示方框"。

### 可选：打包成"单文件工厂镜像"

给不熟悉偏移的人分发时，可以把 5 个文件合成一个（烧到 `0x0` 即可）：

```powershell
# 用 ESP-IDF 自带的 python 环境执行（esptool v5 起子命令是 merge-bin）
python -m esptool --chip esp32s3 merge-bin -o build\XT60-Power-Meter-full.bin `
  --flash-mode dio --flash-size 16MB --flash-freq 80m `
  0x0      build\bootloader\bootloader.bin `
  0x8000   build\partition_table\partition-table.bin `
  0xf000   build\ota_data_initial.bin `
  0x20000  build\XT60-Power-Meter-Firmware.bin `
  0x830000 build\storage.bin
```

产物 `build\XT60-Power-Meter-full.bin` 是**完整 16 MB**（分区之间用 `0xFF` 填充，
属正常现象），烧到偏移 `0x0`。文件大、烧录慢，但最不容易烧错。

---

## 2. 烧录方式一：`idf.py flash`（开发时最常用）

依赖已装好的 ESP-IDF 环境。`build/flash_args` 里记录了全部文件与偏移，
`idf.py flash` 会一次性按正确偏移写入。

```powershell
# 先确认设备串口（Windows 设备管理器，或：
#   Get-CimInstance Win32_PnPEntity | Where-Object Name -match 'COM\d+'
# ESP32-S3 原生 USB 串口通常显示为 "USB 串行设备 (COMx)"）
idf.py -p COM3 flash          # 烧录全部 5 个文件
idf.py -p COM3 monitor        # 查看串口日志（115200）
idf.py -p COM3 flash monitor  # 烧完立刻看日志
```

本仓库提供两个 Windows 一键脚本（内部会自行设置 `IDF_PATH`）：

```powershell
.\build_fw.bat                # 等价 idf.py build
.\flash_fw.bat                # 等价 idf.py -p COM3 flash
.\flash_fw.bat COM5           # 指定串口
```

- 烧录波特率可用环境变量调：`$env:ESPBAUD="921600"`（默认已按 921600 走；
  若你的线材/驱动不稳，先降到 `460800` 或 `115200`）。
- 只烧 app（省时间，用于日常改代码）：`idf.py -p COM3 app-flash`
  —— 但**改字体/分区表时不能只用它**。

---

## 3. 烧录方式二：ESPConnect（网页工具，免安装）

[ESPConnect](https://thelastoutpostworkshop.github.io/ESPConnect/) 是运行在浏览器里的
ESP 烧录/串口工具，通过 **Web Serial** 直接与芯片通信，不需要装 ESP-IDF。
适合：给同事/用户刷一块板、或者手边只有一台干净电脑。

**前提条件**

1. 桌面版 **Chrome 或 Edge**（Web Serial 在移动端 Safari/Firefox 上不可用）；
2. 用 USB 数据线连接设备（不是只能充电的线），页面里能选到串口；
3. **先关掉其他占用串口的程序**（`idf.py monitor`、串口助手、Arduino IDE 等）；
4. 建议用本仓库构建出的 `build/` 产物，或发布页下载的固件包。

**操作步骤（以工具当前界面为准，按钮名为英文）**

1. 打开上述网址 → 点 **Connect**，在浏览器弹窗里选择设备串口（如 `COM3`），
   波特率选 `115200` 连接（连上后工具会自动进入下载模式并识别芯片）；
2. 切到 **Program / Flash** 区域，有两种填法：
   - **单文件（最简单）**：文件选 `XT60-Power-Meter-full.bin`，偏移填 `0x0`；
   - **分文件（更快）**：逐个添加 5 行文件 + 偏移，见第 1 节表格
     （`0x0` / `0x8000` / `0xf000` / `0x20000` / `0x830000`）；
3. 确认 Flash 参数：**Mode = DIO**、**Size = 16MB**、**Freq = 80MHz**
   （与本工程 `sdkconfig.defaults` 一致，选错会起不来）；
4. 点 **Program / Flash**，等待进度完成（分文件方式通常比单文件快很多，
   因为只写实际内容、不写 0xFF 填充）；
5. 完成后点 **Reset / Disconnect**，或拔插 USB 重新上电；
6. 若要用网页控制台：手机/电脑连设备热点或同一路由器，浏览器打开设备 IP。

**常见问题**

| 现象 | 原因与处理 |
|---|---|
| 点 Connect 没有串口 | 线是充电线 / 缺 USB 串口驱动 / 被其他程序占用 → 换线、关掉串口助手 |
| 连接超时、识别不到芯片 | 手动进入下载模式：**按住 BOOT(K1) 不放 → 点一下 RESET → 松开 BOOT**，再点 Connect |
| 烧完不启动、串口无输出 | Flash 参数选错（必须 DIO/16MB/80MHz）；或只烧了 app 没烧 bootloader/分区表 |
| 中文全是方框 | 没烧 `storage.bin`（字体在 storage 分区）；请全量烧录 |
| 烧录中掉线 | 降低波特率（如 460800/115200），换短一点的 USB 线 |

---

## 4. 该烧哪些文件？一张判断表

| 你改了什么 | 最少需要烧 |
|---|---|
| 业务代码 / 网页控制台 / UI | `XT60-Power-Meter-Firmware.bin`（或网页 OTA） |
| 字库（`tools/fonts/make_fonts.py` 重新生成） | **全量 5 个文件**（关键是 `storage.bin`） |
| `partitions.csv` | `bootloader.bin` + `partition-table.bin` + app（建议全量） |
| `sdkconfig.defaults`（Flash/PSRAM/CPU 等） | 建议全量（bootloader 会受影响） |
| 换新板子 / 首次烧录 | 全量 |

**判断口诀**：拿不准就全量烧。全量烧一次几十秒，比"少烧一个文件排查半小时"划算。

---

## 5. OTA 升级（不需要数据线）

设备连上路由器后，在网页控制台的设置/系统区域上传 `XT60-Power-Meter-Firmware.bin`
即可升级（写入另一个 OTA 槽，失败自动回滚）。

**必须知道的两条限制**：

1. OTA **只更新 app**（`ota_0/ota_1`），**不会更新字体**（在 `storage` 分区）；
2. OTA **不会改 bootloader/分区表**。因此：只要是"改字库、改分区、换大版本"，
   就必须插 USB 全量烧录。

---

## 6. 烧录后如何确认成功

1. 串口 115200 能看到启动日志（`app_init: App version: 0.6.x`）；
2. 日志里出现 `fs_storage: 挂载成功：总计 7346 KB，已用 4208 KB（57.3%）`
   （数字随字库版本变化，关键是**没有** `字体文件打不开` 的告警）；
3. 出现 `lvgl_port: 中文字体加载完成：16/16 个`；
4. 屏幕进入主界面、中文无方框；
5. 网页 `GET /api/info` 返回的 `version` 与新烧录版本一致。
