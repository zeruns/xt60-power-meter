# -*- coding: utf-8 -*-
"""
make_splash_font.py — 生成启动页专用小子库（嵌入固件，不占 SPIFFS）

背景（v0.6.9）：
  启动页原先要等 SPIFFS 上的 4MB 全量 Noto 字库加载完才能显示中文，
  首帧只能先显示 ASCII。本脚本把启动页实际用到的字符做成极小子集，
  生成 C 字节数组直接编进固件；LVGL 启动时同步创建 12/14/16/40px
  四个字体实例，首帧即可完整显示中文（不读 SPIFFS）。

字符集来源（自动收集，新增启动页文案后重新运行即可）：
  · main/app_init.c 的 s_boot_steps[] / s_selftest_items[] 中文 label；
  · components/services/ui/src/screens/splash.c 的全部字符串字面量；
  · 固定补充的启动页文案 + ASCII 可打印字符。

输出：
  components/middleware/lvgl_port/src/splash_font_data.c
  components/middleware/lvgl_port/src/splash_font_data.h

用法：
    python tools/fonts/make_splash_font.py
依赖：
    pip install fonttools requests
"""
import io
import os
import re

import make_fonts as mf  # 复用 Noto VF 下载/缓存与子集参数（该模块 main 有 __main__ 保护）

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
APP_INIT_C = os.path.join(ROOT, "main", "app_init.c")
SPLASH_C = os.path.join(ROOT, "components", "services", "ui", "src", "screens", "splash.c")
OUT_DIR = os.path.join(ROOT, "components", "middleware", "lvgl_port", "src")
OUT_C = os.path.join(OUT_DIR, "splash_font_data.c")
OUT_H = os.path.join(OUT_DIR, "splash_font_data.h")

WEIGHTS = [("reg", 500), ("bold", 700)]

# 固定补充：不依赖源码扫描也必须存在的启动页文案字符
EXTRA_TEXT = (
    "XT60功率计作者博客：blog.zeruns.com固件版本"
    "正在准备界面…正在启动应用…硬件自检…自检"
    "系统总线状态指示蜂鸣器系统状态配置存储设备控制数据通道按键"
    "字库文件显示屏触摸屏图形界面姿态传感器屏幕方向"
    "电池检测温湿度传感器测量前端测量服务温湿度姿态电池触摸"
    "（）【】/：%..0123456789"
)

_STR_PAT = re.compile(r'"((?:[^"\\]|\\.)*)"')
# 初始化表/自检表都是 { "英文名", "中文label", ... }
_TABLE_PAT = re.compile(r'\{\s*"[^"]+"\s*,\s*"([^"]+)"\s*,')


def collect_text():
    chars = set(chr(c) for c in range(0x20, 0x7F))
    chars.update(EXTRA_TEXT)

    with open(APP_INIT_C, encoding="utf-8") as f:
        src = f.read()
    labels = _TABLE_PAT.findall(src)
    for s in labels:
        chars.update(s)

    with open(SPLASH_C, encoding="utf-8") as f:
        src = f.read()
    for s in _STR_PAT.findall(src):
        chars.update(s)

    # 只保留可打印字符，去掉误抓到的转义反斜杠
    chars.discard("\\")
    return "".join(sorted(chars)), len(labels)


def write_c_array(f, name, data):
    f.write(f"const uint8_t {name}[] = {{\n")
    per_line = 16
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
    f.write("};\n")
    f.write(f"const size_t {name}_len = sizeof({name});\n\n")


def main():
    from fontTools import subset
    from fontTools.varLib import instancer
    from fontTools.ttLib import TTFont

    text, label_count = collect_text()
    unicodes = {ord(c) for c in text}
    print(f"启动页文案字符数: {len(text)}（来自 app_init 标签 {label_count} 个）")

    vf_path = mf.download_vf()
    blobs = []
    for suffix, weight in WEIGHTS:
        font = TTFont(vf_path, recalcBBoxes=False, lazy=True)
        inst = instancer.instantiateVariableFont(font, {"wght": weight}, inplace=True)

        ss = subset.Subsetter(options=subset.Options(
            layout_features=[],
            name_IDs=[1, 2, 4, 6],
            name_legacy=False,
            notdef_outline=True,
            recalc_bounds=True,
            recalc_timestamp=False,
            drop_tables=["DSIG", "FFTM"],
            glyph_names=False,
        ))
        ss.options.layout_closure = False
        ss.options.drop_tables += ["GSUB"]
        ss.populate(unicodes=unicodes)
        ss.subset(inst)

        buf = io.BytesIO()
        inst.save(buf)
        data = buf.getvalue()
        blobs.append((suffix, data))
        print(f"  {suffix}: {len(data)/1024:.0f} KB，字形 {len(inst.getGlyphOrder())}")

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write("/**\n"
                " * @file splash_font_data.h\n"
                " * @brief 启动页专用 Noto Sans SC 子集（自动生成，勿手改）\n"
                " *\n"
                " * 生成命令：python tools/fonts/make_splash_font.py\n"
                " */\n"
                "#pragma once\n\n"
                "#include <stddef.h>\n"
                "#include <stdint.h>\n\n"
                "extern const uint8_t splash_font_reg_ttf[];\n"
                "extern const size_t splash_font_reg_ttf_len;\n"
                "extern const uint8_t splash_font_bold_ttf[];\n"
                "extern const size_t splash_font_bold_ttf_len;\n")

    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("/**\n"
                " * @file splash_font_data.c\n"
                " * @brief 启动页专用 Noto Sans SC 子集（自动生成，勿手改）\n"
                " *\n"
                " * 只包含启动页/初始化进度用到的字符，随固件编进 app 分区；\n"
                " * 完整 GB2312 字库仍在 SPIFFS，由 lvgl_port 后台异步加载。\n"
                " *\n"
                " * 生成命令：python tools/fonts/make_splash_font.py\n"
                " * 授权：Noto Sans SC，SIL OFL 1.1（见 THIRD_PARTY.md）\n"
                " */\n"
                "#include \"splash_font_data.h\"\n\n")
        for suffix, data in blobs:
            write_c_array(f, f"splash_font_{suffix}_ttf", data)

    print(f"\n输出: {OUT_C}")
    print(f"输出: {OUT_H}")
    print("重新编译固件后生效（子集在 app 分区，随 OTA 一起更新）。")


if __name__ == "__main__":
    main()
