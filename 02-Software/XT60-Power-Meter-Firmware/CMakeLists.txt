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

project(XT60-Power-Meter-Firmware)
