# XT60 功率计固件 —— 顶层工程定义
#
# 组件发现规则（ESP-IDF 只扫描一层子目录）：
#   main/                         -> 自动发现
#   components/bsp/               -> 自动发现（components 为默认扫描目录）
#   components/{drivers,middleware,services}/<组件>/ -> 由 EXTRA_COMPONENT_DIRS 注册
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/drivers"
    "${CMAKE_CURRENT_LIST_DIR}/components/middleware"
    "${CMAKE_CURRENT_LIST_DIR}/components/services")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 版本号唯一来源：version.txt（发版时打 git tag 与之对应）。
# 在 project() 之前定义 PROJECT_VER，ESP-IDF 会将其写入 app_desc（固件版本），
# 否则会回退到 git describe 或默认值 1。
file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)
# 【v0.6.3 修复】把 version.txt 声明为**配置依赖**：否则它只是被 file(READ)
# 读一次，改版本号后增量 `idf.py build` 不会重新配置，固件里的版本号会一直
# 停留在上一次配置时的值（实测：改成 0.6.3 后 /api/info 仍报 0.6.2，必须
# 手工 reconfigure 才生效）。加上这一行后，任何一次改版本号的构建都会自动
# 重新配置并刷新 app_desc，避免"发了新包但版本号没变"。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/version.txt")

project(XT60-Power-Meter-Firmware)
