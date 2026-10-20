# 第三方组件许可清单

本固件（MIT，见 [LICENSE](LICENSE)）在构建时会引入以下第三方组件。
版本号以仓库根目录 `dependencies.lock` 为准（ESP 组件管理器锁定），
字体版本以 `tools/fonts/make_fonts.py` 所用字体文件为准。

| 组件 | 版本 | 许可证 | 来源 |
|---|---|---|---|
| lvgl/lvgl | 9.6（lock 记录 `9.6.0~1`） | MIT | https://github.com/lvgl/lvgl |
| espressif/esp_lvgl_port | 2.9.0 | Apache-2.0 | https://github.com/espressif/esp-bsp （组件注册表：https://components.espressif.com ） |
| espressif/esp_lcd_spd2010 | 1.0.2 | Apache-2.0 | https://components.espressif.com |
| espressif/cmake_utilities | 0.5.3 | Apache-2.0 | https://components.espressif.com |
| Noto Sans SC 字体 | — （Google Fonts 发行版，子集化后随固件烧录） | SIL Open Font License 1.1 | https://fonts.google.com/noto/specimen/Noto+Sans+SC |

## 说明

- **lvgl/lvgl**：图形库，MIT 许可，随固件整体以 MIT 发布时可保留其版权声明即可。
- **esp_lvgl_port / esp_lcd_spd2010 / cmake_utilities**：乐鑫官方组件，
  Apache-2.0 许可，源码经 ESP 组件管理器下载到 `managed_components/`（不入库）。
  Apache-2.0 与本项目 MIT 兼容，再分发时需保留其 LICENSE 与 NOTICE 声明。
- **Noto Sans SC**：Google Fonts 提供的开源中文字体，SIL OFL 1.1。
  本项目用 `tools/fonts/make_fonts.py` 做子集化生成 C 数组（`components/services/fs_storage` / UI 字体源文件）。
  OFL 允许自由使用与再分发，但**不得单独出售字体**；修改/衍生字体亦须以 OFL 发布。
  许可全文：https://openfontlicense.org/
- **ESP-IDF 及其内含组件**（FreeRTOS、esp-idf 各 driver 等）由乐鑫以 Apache-2.0 等许可提供，
  随工具链分发、不随本仓库再分发，故不在本清单逐项列出。
- 依赖的实际版本与哈希校验值请查阅 `dependencies.lock`；升级依赖后请同步更新本表。
