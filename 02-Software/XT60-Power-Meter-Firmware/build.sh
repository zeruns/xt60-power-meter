#!/usr/bin/env bash
# ============================================================
# XT60 功率计固件 —— Linux / macOS 构建脚本（简版）
#
# 用法：
#   export IDF_PATH=/path/to/esp-idf   # 必须先 export，脚本不做安装探测
#   ./build.sh
#
# 说明：本脚本只负责构建；烧录请用 idf.py -p /dev/ttyUSB0 flash。
# ============================================================
set -e

if [ -z "${IDF_PATH}" ]; then
    echo "错误：未设置 IDF_PATH。请先安装 ESP-IDF v6.1 并执行：" >&2
    echo "    export IDF_PATH=/path/to/esp-idf" >&2
    exit 1
fi

# 加载 idf.py 环境（工具链 PATH 等）
# shellcheck disable=SC1091
. "${IDF_PATH}/export.sh"

idf.py build
