# XT60 Power Meter · ESP32-S3 Color-Screen Voltage / Current / Power Meter

> Plug an XT60 connector inline to measure DC voltage, current, power and energy.
> A 1.83-inch IPS color LCD shows readings and waveforms in real time.
> Built-in rechargeable battery and a WiFi web console. **Hardware, firmware and enclosure are fully open-source.**

[![License: CERN OHL-S v2](https://img.shields.io/badge/hardware-CERN%20OHL--S--2.0-blue)](LICENSE)[![Firmware: MIT](https://img.shields.io/badge/firmware-MIT-green)](02-Software/XT60-Power-Meter-Firmware/LICENSE)![MCU](https://img.shields.io/badge/MCU-ESP32--S3-orange)![ADC](https://img.shields.io/badge/ADC-ADS131M02%2024bit-yellow)![Firmware](https://img.shields.io/badge/firmware-v0.6.9-brightgreen)

> 🎬 **Demo video**: to be added (Bilibili link)
> 📦 **OSHWHub / LCEDA open-hardware project**: to be added
> 🔗 **Repository**: [https://gitee.com/zeruns/xt60-power-meter](https://gitee.com/zeruns/xt60-power-meter)
> 💬 Electronics community: [https://bbs.eeclub.top/](https://bbs.eeclub.top/)
> 📖 Author's blog: [https://blog.zeruns.com/](https://blog.zeruns.com/)
> 🇨🇳 **中文文档**: [README.md](README.md)

![](https://tc2.zeruns.com/Pic/2026/10/IMG_20261031_163238%281%29_f6cebf8d83645903a56acc390aabcf88.webp)

---

## Table of Contents

- [1. Project Overview](#1-project-overview)
  - [Highlights](#highlights)
  - [Key Specifications](#key-specifications)
- [2. Gallery](#2-gallery)
  - [2.1 Appearance](#21-appearance)
  - [2.2 Internal Structure](#22-internal-structure)
  - [2.3 Screen UI](#23-screen-ui)
  - [2.4 Web Console](#24-web-console)
- [3. Hardware Design](#3-hardware-design)
  - [3.1 System Architecture](#31-system-architecture)
  - [3.2 Power & Battery Management](#32-power--battery-management)
  - [3.3 Voltage Sensing](#33-voltage-sensing)
  - [3.4 Current Sensing](#34-current-sensing)
  - [3.5 MCU, Display & Sensors](#35-mcu-display--sensors)
  - [3.6 PCB Design](#36-pcb-design)
- [4. Software Design](#4-software-design)
  - [4.1 Firmware Architecture](#41-firmware-architecture)
  - [4.2 Measurement Pipeline](#42-measurement-pipeline)
  - [4.3 Local UI](#43-local-ui)
  - [4.4 Web Console & API](#44-web-console--api)
  - [4.5 Engineering](#45-engineering)
- [5. Enclosure & Assembly](#5-enclosure--assembly)
- [6. Build & Reproduction](#6-build--reproduction)
  - [6.1 BOM & Cost](#61-bom--cost)
  - [6.2 PCB Fabrication & Stencil](#62-pcb-fabrication--stencil)
  - [6.3 Soldering](#63-soldering)
  - [6.4 Flashing](#64-flashing)
  - [6.5 Calibration](#65-calibration)
- [7. Usage](#7-usage)
- [8. Roadmap](#8-roadmap)
- [9. Known Issues](#9-known-issues)
- [10. Repository Structure](#10-repository-structure)
- [11. Documentation Index](#11-documentation-index)
- [12. Contributing](#12-contributing)
- [13. License](#13-license)
- [14. Disclaimer](#14-disclaimer)
- [Recommended Reading](#recommended-reading)

---

## 1. Project Overview

When flying RC models, testing drones or cycling battery packs, you often need a power meter that
**plugs directly into the XT60 loop**: you want to see the voltage sag under high current, capture the
inrush waveform when connecting a load, and ideally sync the data to a PC. Commercial units either
have tiny screens, no waveform view, or keep data locked on the device — so this project exists.

The XT60 Power Meter is built around an **ESP32-S3** and a **TI ADS131M02 24-bit dual-channel
simultaneous-sampling ADC (4 kSPS)**, combined with a 1 mΩ four-terminal Kelvin shunt and a 101:1
precision divider. It measures DC circuits up to **0–90 V, ±30 A rated (±75 A peak)**.
A 1.83-inch IPS color LCD runs an **LVGL 9** UI with three pages (measure / waveform / settings).
Once connected to WiFi, the **web console** lets you watch live data and waveforms, change settings
and export CSV from any browser, and a complete HTTP API is available for your own host software
or automation scripts.

### Highlights

| Category | Features |
| :-: | :-- |
| Measurement | Average voltage / current (DC), instantaneous and average power, accumulated energy (Wh), capacity (Ah), runtime |
| Sampling | ADS131M02 24-bit dual-channel simultaneous sampling at 4 kSPS, CRC frame validation drops corrupted frames |
| Waveform | Oscilloscope-style dual-axis chart, V/I/P modes, six time windows from 1 s to 10 min, auto-ranging on 1-2-5 grid values |
| Interaction | Capacitive touch + two buttons, pull-up quick menu on the main page, three swipeable pages, IMU auto-rotation, automatic dimming |
| Alarms | Configurable over-voltage (max 90 V) / over-current (max 70 A) / over-power (max 6300 W) thresholds plus always-on over-temperature protection (dual temperature sources, ≥65 °C) |
| Connectivity | WiFi STA/AP provisioning, five-tab web console (live / waveform / settings / API / logs), 15 Hz WebSocket streaming |
| Power | XT60 input / USB / Li-ion battery, TPS2116 automatic USB-battery priority switchover, TP4057 charging |
| Engineering | Dual OTA partitions with rollback, coredump capture, PC-side LVGL UI simulator, host unit tests |

### Key Specifications

| Item | Value |
| --- | --- |
| MCU | ESP32-S3-WROOM-1-N16R8 (16 MB Flash + 8 MB octal PSRAM, dual-core 240 MHz) |
| Metering ADC | TI ADS131M02, 24-bit Σ-Δ, dual-channel simultaneous sampling, 4 kSPS |
| Voltage range | **0–90 V** design range (the divider is theoretically ±121 V, limited to 90 V by the 100 V maximum auxiliary supply input) |
| Current range | **±75 A peak** (ADC full scale with 1 mΩ + PGA 16x); **±30 A rated** (5 W shunt allows 70 A, XT60 connector rated 30 A) |
| Connector | XT60 (rated 30 A; builders may redesign the bottom board with XT90 to use the full 70 A shunt capability) |
| Display | 1.83-inch IPS, 240×284 (landscape 284×240), NV3030B controller, QSPI; CST816D capacitive touch |
| Sensors | SC7A20HTR 3-axis accelerometer (auto-rotate), AHT20 temperature / humidity |
| Power | XT60 input (SY8511BADC 3.3 V / 1 A), USB/battery (TLV62084 3.3 V / 2 A), TPS2116 automatic USB-battery priority switch, TP4057 400 mA charging |
| Wireless | WiFi 2.4 GHz (STA/AP), HTTP + WebSocket |
| Enclosure | SolidWorks sources + 3MF print files (top cover / middle frame / bottom cover) |

> ⚠️ This device is intended for hobbyist measurement and debugging and is **not a certified
> metering instrument**. Currents below 2 mA are displayed as 0 (small-signal deadband);
> the waveform page still shows mA-level detail. See [7. Usage](#7-usage) for details.
>
> When powered from the XT60 input, the minimum start-up voltage is 5 V.
>
> The software was developed entirely with AI assistance.

---

## 2. Gallery

### 2.1 Appearance

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_040258_3d78c7efd72f1373efcd3bacb8796a8f.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_040311_711ff4b0826e90d076c9959e9fb6f459.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_040708_33f38b9f22f6c5d8b28026537e9d76ab.webp)

The enclosure is 3D-printed in three parts (top cover, middle frame, bottom cover) and held by screws.

The front is a 1.83-inch IPS screen; on its left are the system status LED and the reset button.
Two physical buttons (K1/K2) are provided on the body — swipe up on the screen or long-press K2 to
open the quick menu.

The switch to the right of the USB-C port selects the power source (battery & USB, or XT60).

### 2.2 Internal Structure

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261021_225034_b2862a188c01ba6d2c8e752f381a37b5.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261021_231338_754f05b2abf53e373bfc09801c68e31f.webp)

The device uses a **two-board stacked** construction: the top board carries the MCU, display,
sensors, buttons, voltage divider and power management; the bottom board carries the XT60 input/output
and the 1 mΩ shunt. The battery sits on the back of the top board, sandwiched between the two boards;
fix it with double-sided tape or foam, and preferably wrap it in insulating paper.

### 2.3 Screen UI

| Main measurement page | Oscilloscope-style waveform page |
| :---: | :---: |
| ![Measurement page](https://tc2.zeruns.com/Pic/2026/11/ui_measure_8ab2c86641b538251224d69956f2779e.png) | ![Waveform page](https://tc2.zeruns.com/Pic/2026/11/ui_wave_1a829869dd821102645d119c8c5ea365.png) |
| Four rows of large digits (voltage / current / power / energy); energy, temperature/humidity and peak values at the bottom | V/I/P multi-mode dual-axis curves, 1 s–10 min time windows, auto-ranging |

| Settings page | Quick menu (pull up) |
| :---: | :---: |
| ![Settings page](https://tc2.zeruns.com/Pic/2026/11/ui_settings_33b8b3b590df65d29e27988189f20f94.png) | ![Quick menu](https://tc2.zeruns.com/Pic/2026/11/ui_quick_menu_a5f4a9ac0d1ef87bb35d6ec92ffd0f88.png) |
| Second-level pages for measurement settings, display & sound, WiFi, alarms and energy statistics | Clear energy statistics (with confirmation), data refresh rate 1–30 Hz |

> The UI images above come from the project's PC-side LVGL simulator (`tools/ui_sim`) and match
> real-device rendering.

### 2.4 Web Console

![](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-10-25_01-59-40_84cf88d80e3008780c243f9abd56fd26.webp)

![](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-10-25_01-58-54_d3a2d4055abff141ace6b126fe2e7eaf.webp)

![](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-10-25_02-01-40_39f49c40728c179b65d2cce257b132bc.webp)

Open the device IP in a phone or PC browser — no app installation required. If the network is
unavailable, the local UI keeps working normally.

---

## 3. Hardware Design

### 3.1 System Architecture

![XT60 Power Meter hardware block diagram](https://tc2.zeruns.com/Pic/2026/11/block-diagram_4e1dbc1b78441e2d29679fb0b7d7bc6f.webp)

The system consists of six parts:

| Module | Key devices | Responsibility |
| :-: | --- | --- |
| MCU | ESP32-S3-WROOM-1-N16R8 | Sampling scheduling, UI rendering, WiFi, data storage |
| Metering front end | ADS131M02 + LRA8551A | Simultaneous voltage/current sampling, 24-bit, 4 kSPS |
| Display & interaction | NV3030B LCD + CST816D touch + two buttons + buzzer | Local display and operation |
| Sensors | SC7A20HTR + AHT20 | Screen orientation, internal temperature/humidity |
| Power | SY8511BADC / TLV62084 / TPS2116 / TP4057 | Three-input switching, 3.3 V regulation, battery charging |
| Storage & update | 16 MB on-chip Flash + dual OTA partitions | Firmware, font resources, OTA update and rollback |

### 3.2 Power & Battery Management

![Power management schematic](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-11-06_22-22-09_cc631dd51b0ae752e5db4439c9ec5341.webp)

- **Three power inputs**: the XT60 circuit under test, USB-C, and a single-cell Li-ion battery.
  The XT60 input is bucked to 3.3 V by SY8511BADC (1 A max); USB/battery is bucked to 3.3 V by
  TLV62084 (2 A max); TPS2116 switches between USB and battery with USB priority — see the
  schematic for how the two rails are combined.
- TP4057 charges the Li-ion battery at 400 mA; the battery voltage is divided 1:1 into an ESP32 ADC
  for the battery gauge.
- The 3.3 V rail supplies the MCU, LCD, ADC digital side and sensors; analog front-end supply and
  filtering details are in the schematic.

Besides the TPS2116 solution for USB/battery priority switching (V2 board), there is also a
discrete-MOS implementation (V1 board):

![](https://tc2.zeruns.com/Pic/2026/11/image-20261106222543910_7ae153e742438159b5e1acf7ae4ad9b7.png)

### 3.3 Voltage Sensing

![Voltage sensing schematic](https://tc2.zeruns.com/Pic/2026/11/image-20261106222831117_acf0e4e363f391a0d4f6b7ba33af24c9.png)

The voltage channel uses a **101:1 precision divider** (100 kΩ / 1 kΩ, ±0.1% thin-film resistors).
The divided signal is buffered by a zero-drift op-amp (LRA8551A) and fed into ADS131M02 CH1 (PGA 1x).
The divider is theoretically good to 121 V, but the auxiliary supply accepts at most 100 V, so the
**design measurement range is 0–90 V**; one LSB is about 14.5 µV over that range.

### 3.4 Current Sensing

![Shunt and current sensing](https://tc2.zeruns.com/Pic/2026/11/image-20261106222918309_e099710a3e10ec60d1fa0de043bf32b6.png)

![Current sensing schematic](https://tc2.zeruns.com/Pic/2026/11/image-20261106223640128_f7bae675cfd244da20d27731f7d01640.png)

The current channel uses a **1 mΩ / 5 W four-terminal Kelvin shunt**; the sense taps are independent
of the high-current copper path to avoid copper resistance errors. After RC filtering the signal goes
directly into ADS131M02 CH0 (PGA 16x). With PGA 16x the ADC full scale corresponds to
**±75 A peak** (1 LSB ≈ 8.94 µA). The 5 W shunt limits continuous current to 70 A
(70 A → 70 mV drop, 4.9 W dissipation); the XT60 connector is rated 30 A (30 A → 30 mV drop,
0.9 W). Builders who need higher continuous current can redesign the bottom board with an **XT90**
connector.

> Why 1 mΩ: it covers ±75 A peak and 30 A rated while keeping shunt dissipation below 5 W;
> the trade-off is higher small-signal noise (σ ≈ 2 mA per frame), which the firmware handles with
> 200 ms block averaging plus a 2 mA deadband.

### 3.5 MCU, Display & Sensors

![MCU and display schematic](https://tc2.zeruns.com/Pic/2026/11/%E4%B8%BB%E6%8E%A7%E4%B8%8E%E6%98%BE%E7%A4%BA%E5%8E%9F%E7%90%86%E5%9B%BE-1793977952919-14_6701c9fae2ac87c8ed0260deb9e7d04c.webp)

- The ESP32-S3 drives the NV3030B display over **QSPI** (4 data lines); the TE pin is reserved on
  the board but not used by the current firmware (refresh is driven by LVGL's partial flush).
- The CST816D capacitive touch controller uses I2C (0x15); its INT line is GPIO46.
- AHT20 (0x38) and SC7A20HTR (0x19) share the sensor I2C bus.
- Buttons K1 (BOOT) and K2 are active-low; the buzzer is low-side driven from GPIO1 through an AO3400A.
- The status LED on IO39 is active-low and indicates boot/fault states.

### 3.6 PCB Design

![Top board PCB](https://tc2.zeruns.com/Pic/2026/11/IMG_0872_22d390e02a0eba572fcd520400ed8f82.webp)

![Top board bottom layer](https://tc2.zeruns.com/Pic/2026/11/IMG_0890_ab74e525497544224c9c0a947214686a.webp)

![Top board top layer](https://tc2.zeruns.com/Pic/2026/11/IMG_0891_a087c249043ec4415c66ad58e572ae30.webp)

![Bottom board PCB](https://tc2.zeruns.com/Pic/2026/11/IMG_0903_046bb89dd6296045676408ae1e18d249.webp)

Design notes:

- Two stacked boards connected by pin headers/sockets; the display and ICs live on the top board,
  while the high-current path stays on the bottom board.
- The current sense signal is a small differential signal: keep ISENSE traces short and away from
  DC-DC inductors and switching nodes.
- Keep the ADC SPI and DRDY traces short; avoid long parallel runs with clock or switching noise.
- Use wide traces on the high-current path to reduce drop and temperature rise at 30 A rated / 75 A peak.
- Ground and power handling for the analog front end and digital section are documented in the
  schematic and PCB project files.

> PCB project, Gerbers and interactive BOM are under `05-生产文件/`; schematic PDFs are under `01-Hardware/`.

---

## 4. Software Design

### 4.1 Firmware Architecture

The firmware is based on **ESP-IDF v6.1 + LVGL 9.6** and layered as driver / middleware / service / application:

```
main/                 boot flow, task creation, parameter wiring (app_tasks)
components/
├── drivers/          ads131m02 / lcd_nv3030b / key_button / buzzer / imu_sc7a20h ...
├── middleware/       data_store (snapshots) / web_console / wifi_manager / lvgl_port / fs_storage
└── services/         power_measure (measurement core) / ui / settings / orientation / system_state
```

Dual-core split (two Xtensa cores on ESP32-S3):

- **Core 1 (measurement core)**: `task_sample` is driven by the DRDY interrupt — 4 kSPS frame reads,
  CRC checks, code-to-value conversion, waveform ring buffer, 200 ms block statistics and energy
  integration; `task_aux` handles slow sensors such as temperature/humidity and battery.
- **Core 0 (interaction core)**: LVGL rendering, button events, WiFi/HTTP services and web pushes.

### 4.2 Measurement Pipeline

1. ADS131M02 asserts DRDY every 250 µs; SPI DMA reads a 4-word frame (STATUS/CH0/CH1/CRC).
2. After CRC-16/CCITT validation, calibrated coefficients convert codes to voltage V, current A and
   instantaneous power W.
3. Raw samples are written into a three-tier PSRAM ring buffer for the waveform page.
4. Every 200 ms (wall clock) the firmware settles block averages, average power, peak values and
   alarms, and integrates energy/capacity.
5. Every 25 ms a 40 Hz display snapshot is produced in `data_store`; both the UI and the web console
   only read snapshots.

> Details and troubleshooting notes (stable 4 kHz reads, CRC spikes, memory strategy, software
> rotation) are in the [architecture document](02-Software/XT60-Power-Meter-Firmware/docs/architecture.md).

### 4.3 Local UI

- A three-page `tileview`: measure / waveform / settings; it stops at the first and last page.
- Everything is operable with **buttons only**: K1/K2 single-click moves focus, double-click confirms,
  long-press opens/closes menus.
- The waveform page uses a fixed 5×10 grid; auto-ranging snaps to 1-2-5 standard grid values and
  axis labels drop trailing zeros (3 V / 0.2 A instead of 3.00 V / 0.200 A).
- Dialog width adapts to screen orientation (260 px landscape / 216 px portrait), so portrait mode is
  no longer clipped.

### 4.4 Web Console & API

- A single page embedded in the firmware (no external CDN); it works without Internet access.
- WebSocket `/ws` pushes snapshots at 15 Hz; on disconnect it falls back to 100 ms polling and
  reconnects every 5 s.
- Five tabs: live data / waveform / settings / API / logs; settings are read back from the device on open.
- Main endpoints: `/api/state`, `/api/config` (GET/POST), `/api/info`, `/api/wifi/scan`,
  `/api/export.csv`, `/ws`.
- The API tab includes an inline "▶ Test" button that sends requests in place and shows status code,
  latency and response body.

### 4.5 Engineering

- **Dual OTA partitions + rollback**: `ota_update_start(url)` performs a pull-style upgrade into the
  spare partition; a failed boot rolls back to the previous version. (The web console does not expose
  a firmware-upload endpoint yet, so recovery still relies on USB/serial flashing.)
- **coredump**: crash dumps are written to a dedicated partition and can be exported for analysis after reboot.
- **PC-side UI simulator**: `tools/ui_sim` renders the same UI sources on Windows and captures batch
  screenshots for layout regression before flashing.
- **Font subsetting**: `tools/fonts/make_fonts.py` scans all UI strings and generates a GB2312
  level-1 + level-2 font set.

---

## 5. Enclosure & Assembly

![Assembly exploded view](https://tc2.zeruns.com/Pic/2026/11/Snipaste_2026-11-07_01-15-56_5a38d0b5a629dd5d8636f259a4468106.webp)

![Enclosure parts](https://tc2.zeruns.com/Pic/2026/11/IMG_0897_759e97481326c01fa30d892ebc148365.webp)

![Accessories](https://tc2.zeruns.com/Pic/2026/11/IMG_0896_9e810b8cf1a0420ec963f3d021957efb.webp)

The enclosure is designed in SolidWorks; sources are in `04-外壳设计/`, and 3MF print files are in
`05-生产文件/03-外壳3D打印文件/`. It is a three-part design: top cover, middle frame and bottom cover.

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261021_231338-1794078515983-23_754f05b2abf53e373bfc09801c68e31f.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261021_225920_64f47a4f724b664714c0207b6313201f.webp)

Assembly order:

1. Insert heat-set inserts into the 8 screw holes on the middle frame (a soldering iron works well).
2. Fit the bottom board into the bottom cover, place the middle frame on top (note its orientation in
   the photos), then tighten the four screws of the bottom cover.
3. Wrap the battery in insulating paper, plug its lead into the battery connector on the top board,
   and stick the battery to the underside of the top board (the side with the ESP32 module) using
   double-sided tape.
4. Place the top board with the battery onto the middle frame, taking care to align the pin headers.
5. Fit the top cover.
6. Finally attach the screen, fixing it with double-sided tape on its back.

---

## 6. Build & Reproduction

### 6.1 BOM & Cost

- Full BOM: `05-生产文件/02-贴片/01-上板_V2-BOM.xlsx`, `01-下板-BOM.xlsx`.
- Interactive soldering assistant: `04-上板_V2-交互式BOM-焊接辅助.html` in the same folder
  (open in a browser; clicking a designator highlights it).
- Designator maps: `03-上板_V2-位号图.pdf` / `03-下板-位号图.pdf`.

Cost estimate for the main components (prices fluctuate; for reference only):

| Item | Qty | Unit price (CNY) | Notes |
| :-: | :-: | :-: | :-- |
| ESP32-S3-WROOM-1-N16R8 | 1 | 31.6 | MCU & WiFi module |
| ADS131M02 | 1 | 16.8 | 24-bit ADC |
| 1.83-inch capacitive touch LCD, NV3030B | 1 | 14.8 | Ribbon cable included |
| 1 mΩ / 5 W four-terminal Kelvin shunt | 1 | 3.69 | Four-terminal |
| 500 mAh Li-ion battery | 1 | 8.2 | Single cell |
| PCB (top + bottom) | 2 |  | Free prototyping at JLCPCB |
| Enclosure (3D printed) | 1 set | 15 | Top / middle / bottom |

### 6.2 PCB Fabrication & Stencil

- Top board Gerber: `05-生产文件/01-PCB/Gerber_上板_V2.1.zip`.
- Bottom board Gerber: `05-生产文件/01-PCB/Gerber_下板_2026-09-22.zip`.
- Stencil: `05-生产文件/01-PCB/手工焊接用钢网/上板V2钢网.GTS` (for hand soldering; both top and
  bottom apertures are on one sheet — order a laser stencil from Taobao or a local service).

> Suggested board thickness is 1.2 mm; default impedance/stackup process is fine.

### 6.3 Soldering

1. Buy solder paste; align and fix the stencil on one side, then spread the paste.
2. Place the SMD components after pasting.
3. Reflow on a hot plate or with a hot-air gun.
4. Solder the through-hole parts.

### 6.4 Flashing

Windows one-click flashing (requires ESP-IDF v6.1, or a pre-configured environment):

```powershell
cd 02-Software/XT60-Power-Meter-Firmware
.\build_fw.bat			# build the firmware
.\flash_fw.bat COM3     # flash; use the COM port shown in Device Manager
```

You can also flash directly from the browser with **ESPConnect**, without installing ESP-IDF.
See the [build & flash guide](02-Software/XT60-Power-Meter-Firmware/docs/build_and_flash.md) for the
image offset table, all three flashing methods and troubleshooting.

### 6.5 Calibration

1. With no load, go to Settings → Measurement Settings → Current zero calibration; it samples for
   3 seconds and zeroes automatically.
2. Voltage calibration: apply a known reference voltage (e.g. 12.000 V) and correct gain/offset from
   the measured value.
3. Current calibration: connect a reference meter or electronic load in series and correct accordingly.
4. Calibration values are stored in NVS and survive power cycles and firmware upgrades.

---

## 7. Usage

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261022_173809_56f7d3083d82ebe94c2d913f47a490ef.webp)

1. **Wiring**: connect the power source/battery to the XT60 input and the load to the output; observe
   **polarity**. Input and output may be reversed; negative current is displayed.
   Rated ±30 A, short-term peak ±75 A (the rating is limited by the XT60 connector; for sustained
   high current, replace the bottom board with an XT90 version).
2. **Power on**: switch to USB/battery to boot. If the switch is in XT60 mode, applying 5 V or more
   also boots the device. The splash screen shows real initialization progress (including hardware
   self-tests); splash text uses a small font subset embedded in the firmware, so Chinese is complete
   on the very first frame, while the full main-UI font keeps loading in the background.
3. **Measurement**: the main page shows voltage, current, power and energy; **tap the bottom energy
   row to toggle between Wh and Ah**.
4. **Waveform**: swipe to the waveform page; swipe up to open the quick menu and switch time windows
   and V/I/P modes.
5. **Quick menu**: swipe up on the measurement page (or long-press K2) to change the data refresh rate
   or clear energy statistics.
6. **Network setup**: Settings → WiFi & Hotspot → scan for WiFi and enter the password (or use hotspot
   mode: connect a phone/PC to the device hotspot and open the web console to provision). A WiFi icon
   appears in the status bar once connected.
7. **Web console**: open the device IP in a browser; five tabs: live data, waveform, settings, API, logs.
8. **Charging**: connect 5 V to USB-C; TP4057 charges at 400 mA and the status bar shows the battery level.

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261025_011253_b4e3d07804e40ab81382d8fc79149b08.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261025_011344_6d9247c8228a32215e16554c4a73f171.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261025_011412_e02af3aa571a7e24d94e0caee8d4fb5e.webp)

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261025_011421_7e25a38a98eb3925049007300bf238fa.webp)

> A current reading of 0 with no load is **normal**: when |I| < 2 mA the firmware reports 0
> (small-signal deadband) to eliminate last-digit jitter caused by the 1 mΩ shunt noise. The waveform
> page still shows raw samples, so mA-level changes remain visible.

The meter itself draws about 0.7 W.

![](https://tc2.zeruns.com/Pic/2026/11/IMG_20261025_011603_d3efead4ab87642a4cdedd20f82ab0e5.webp)

---

## 8. Roadmap

1. OTA upgrade feature.
2. USB serial host communication plus companion PC software.
3. ...

These are planned when time permits; PRs are welcome.

---

## 9. Known Issues

① When powered from battery/USB, the 3.3 V rail back-feeds into the XT60 connector through the body
diode of the XT60 buck converter's internal MOSFET and the body diode of the power-control MOSFET (Q4).
As a result, about 2.45 V appears on the XT60 connector even with no input connected (3.3 V minus the
MOSFET body-diode drop).

> Possible fixes:
> 1. Add another MOSFET in series after Q4, oriented back-to-back.
> 2. If you do not need XT60-powered operation and do not want to change the circuit, simply do not
>    populate the XT60 buck section and run on battery only.
> 3. If you do not need battery/USB power, just switch to XT60 power mode.

![](https://tc2.zeruns.com/Pic/2026/11/image-20261109013933541_f7bf345408e05552d5e0e115956b14c5.png)

---

## 10. Repository Structure

```
XT60-Power-Meter/
├── 01-Hardware/              Hardware design
│   ├── 01-XT60功率计-立创EDA工程.epro2
│   ├── 02-上板原理图_V1.pdf / V2.pdf
│   └── 02-下板原理图.pdf
├── 02-Software/
│   └── XT60-Power-Meter-Firmware/    ESP-IDF firmware project
│       ├── components/               drivers / middleware / services
│       ├── docs/                     architecture, flashing, pinout
│       └── tools/                    font tools, LVGL UI simulator
├── 03-Datasheet/             Datasheets for all components (CN/EN)
├── 04-外壳设计/              SolidWorks sources (top/middle/bottom, assembly)
├── 05-生产文件/
│   ├── 01-PCB/               Gerbers, stencils
│   ├── 02-贴片/              BOM, designator maps, interactive soldering aid
│   └── 03-外壳3D打印文件/    3MF files
├── images/                   README images
├── LICENSE                   Hardware license (CERN OHL-S v2)
└── README.md
```

---

## 11. Documentation Index

| Document | Content |
| --- | --- |
| [Firmware README](02-Software/XT60-Power-Meter-Firmware/README.md) | Firmware features, hardware summary, build and flashing |
| [Build & Flash Guide](02-Software/XT60-Power-Meter-Firmware/docs/build_and_flash.md) | Five images, three flashing methods, decision table and troubleshooting |
| [Architecture](02-Software/XT60-Power-Meter-Firmware/docs/architecture.md) | Layering, dual-core split, measurement pipeline, lessons learned |
| [Pinout](02-Software/XT60-Power-Meter-Firmware/docs/hardware_pinout.md) | All GPIOs and peripheral mappings |
| [Contributing Guide](02-Software/XT60-Power-Meter-Firmware/CONTRIBUTING.md) | Commit conventions, code style, comment requirements |
| [Third-Party Licenses](02-Software/XT60-Power-Meter-Firmware/THIRD_PARTY.md) | Licenses for LVGL / esp_lvgl_port / fonts, etc. |

---

## 12. Contributing

1. Fork this repository and create a `feat/xxx` or `fix/xxx` branch.
2. Commit messages follow Conventional Commits, written in **Chinese**.
3. For firmware changes, run the `tools/ui_sim` scenario regression first, then verify on real hardware.
4. Open a Pull Request describing the motivation, how it was verified, and the impact.

For hardware changes (schematic/PCB/enclosure), include a change description and photos. When
reporting measurement anomalies, please provide serial logs and the `/api/info` version information.

---

## 13. License

- **Hardware design** (schematics, PCB, enclosure, production files): [CERN OHL-S v2](LICENSE)
  — a strongly reciprocal open-hardware license; derivative hardware must be published under the same license.
- **Firmware source** (`02-Software/`): [MIT](02-Software/XT60-Power-Meter-Firmware/LICENSE).
- Third-party components (LVGL, esp_lvgl_port, Noto Sans SC, etc.) retain their own licenses; see
  [THIRD_PARTY.md](02-Software/XT60-Power-Meter-Firmware/THIRD_PARTY.md).

---

## 14. Disclaimer

- Measurement results from this device are **not valid for metrological certification**.
- **90 V / ±75 A peak (±30 A rated)** involves hazardous voltages and currents. When wiring, soldering
  and testing, guard against short circuits, electric shock and reverse polarity. Use suitable wire
  gauge and connectors for high-current tests.
- The author assumes no liability for any direct or indirect loss caused by the use of this hardware
  or firmware.
- Issues and improvement suggestions are welcome via GitHub/Gitee Issues.

---

## Recommended Reading

- **Cost-effective and cheap VPS / cloud server recommendations:** [https://blog.zeruns.com/archives/383.html](https://blog.zeruns.com/archives/383.html)
- Minecraft server setup tutorial: [https://blog.zeruns.com/tag/mc/](https://blog.zeruns.com/tag/mc/)
- [Open-source] SC8703-based buck-boost adjustable DC power supply, adjustable voltage/current, PD fast-charge input: [https://blog.zeruns.com/archives/929.html](https://blog.zeruns.com/archives/929.html)
- [Open-source] 24V/3A flyback switching power supply (UC3842-based, with circuit and transformer design calculations): [https://blog.zeruns.com/archives/910.html](https://blog.zeruns.com/archives/910.html)
- **STM32-based synchronous rectification buck-boost digital power supply (open-source):** [https://blog.zeruns.com/archives/791.html](https://blog.zeruns.com/archives/791.html)
- Open-source 140W+65W buck-boost PD3.1 charger module (2C+1A ports), IP6557+IP6538, 205W desktop charger: [https://blog.zeruns.com/archives/801.html](https://blog.zeruns.com/archives/801.html)
- **Open-source Type-C docking station: 4× 10Gbps USB-A + 2.5G Ethernet + card reader:** [https://blog.zeruns.com/archives/868.html](https://blog.zeruns.com/archives/868.html)
