# XT60 功率计 · LVGL UI 模拟器（源码级，PC 端）
#
# 目标：用**与真机相同的源码**（LVGL 9.6 + components/services/ui 下的界面代码）
#       与**同一份字体**在 PC 上渲染，headless 输出截图（out/*.bmp）。
# 工具链：MSVC（Visual Studio Build Tools 2022 自带 cl + CMake + Ninja），
#         无需额外安装任何依赖。
#
# 用法（见 build.ps1 / capture.py）：
#   powershell -File build.ps1     # 构建 + 运行 + 转 PNG（一键）
cmake_minimum_required(VERSION 3.20)
project(xt60_ui_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 项目源码为 UTF-8（含中文注释/字符串），MSVC 必须显式指定
if(MSVC)
  add_compile_options(/utf-8)
endif()

# ---- 路径（相对本目录） ----
set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
get_filename_component(ROOT_DIR "${ROOT_DIR}" ABSOLUTE)
set(LVGL_DIR "${ROOT_DIR}/managed_components/lvgl__lvgl")
set(UI_DIR "${ROOT_DIR}/components/services/ui/src")
set(FONT_DIR "${ROOT_DIR}/components/middleware/fs_storage/spiffs_image/fonts")

if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
  message(FATAL_ERROR "找不到 LVGL 源码：${LVGL_DIR}（请先 idf.py reconfigure 拉取组件）")
endif()
if(NOT EXISTS "${FONT_DIR}/notosc_reg.ttf")
  message(FATAL_ERROR "找不到字体：${FONT_DIR}/notosc_reg.ttf")
endif()

# ---- LVGL 静态库（PC 端直接编译源码，按 lv_conf.h 裁剪模块） ----
file(GLOB_RECURSE LVGL_SOURCES "${LVGL_DIR}/src/*.c")
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC "${LVGL_DIR}" "${LVGL_DIR}/src")
target_compile_definitions(lvgl PUBLIC "LV_CONF_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h\"")
if(MSVC)
  target_compile_options(lvgl PRIVATE /W1) # 第三方源码降噪
endif()

# ---- 模拟器可执行 ----
add_executable(ui_sim
  src/sim_main.c
  src/sim_scenes.c
  src/lvgl_port_sim.c
  src/stubs/stubs.c
  src/mocks/mock_data_store.c
  src/mocks/mock_lcd.c
  src/mocks/mock_settings.c
  src/mocks/mock_orientation.c
  src/mocks/mock_buzzer.c
  src/mocks/mock_power_measure.c
  src/mocks/mock_wifi_manager.c # v0.6.0：WiFi 页与状态栏信号条依赖 wifi_manager
  # ↓ 与真机完全相同的界面源码（设置页/波形页/测量页/启动页/导航）
  "${UI_DIR}/ui_nav.c"
  "${UI_DIR}/screens/splash.c"
  "${UI_DIR}/screens/screen_measure.c"
  "${UI_DIR}/screens/screen_wave.c"
  "${UI_DIR}/screens/screen_settings.c"
)

target_include_directories(ui_sim PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"     # 模拟器版 lvgl_port.h / lcd_nv3030b.h（覆盖真机同名头）
  "${CMAKE_CURRENT_SOURCE_DIR}/src/stubs"   # esp_err.h / esp_log.h / esp_app_desc.h / freertos（桩）
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${UI_DIR}" "${UI_DIR}/screens"
  "${ROOT_DIR}/components/services/ui/include"
  "${ROOT_DIR}/components/services/settings/include"
  "${ROOT_DIR}/components/services/orientation/include"
  "${ROOT_DIR}/components/services/power_measure/include"
  "${ROOT_DIR}/components/drivers/buzzer/include"
  "${ROOT_DIR}/components/drivers/key_button/include"
  "${ROOT_DIR}/components/middleware/data_store/include"
  # v0.6.0：WiFi 页（模式/扫描/RSSI/键盘配网）与状态栏信号条依赖它
  "${ROOT_DIR}/components/middleware/wifi_manager/include"
)

target_compile_definitions(ui_sim PRIVATE
  _CRT_SECURE_NO_WARNINGS # MSVC 不再对 strncpy 等传统 CRT 函数告警（与 ESP-IDF 语义一致）
  "SIM_FONT_REG_PATH=\"${FONT_DIR}/notosc_reg.ttf\""
  "SIM_FONT_BOLD_PATH=\"${FONT_DIR}/notosc_bold.ttf\""
  "SIM_OUT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/out\""
  "SIM_FW_VERSION=\"0.2.0\""
)

if(MSVC)
  target_compile_options(ui_sim PRIVATE /W3)
endif()

target_link_libraries(ui_sim PRIVATE lvgl)