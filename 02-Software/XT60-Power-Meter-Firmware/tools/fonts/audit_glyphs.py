# -*- coding: utf-8 -*-
"""audit_glyphs.py — UI 文案与字体子集的缺字审计（字库缺失闭环校验）

做两件事：
  1. 扫描 components 下所有 C 源码里的字符串字面量，
     收集其中出现的全部字符（去重）；
  2. 与工程内实际烧录的 Noto Sans SC 子集 TTF（notosc_reg.ttf）的 cmap 比对，
     列出"源码要用、但字体里没有"的字符（即真机上会显示成方框的字）。

【v0.6.2 需求 3】扫描范围与 tools/fonts/make_fonts.py 保持一致，都扩大到
整个 components（不再只扫 ui/src），这样审计口径与字体子集口径完全对齐，
不会出现"字体工具漏扫、审计也漏扫"的双重盲区。

用法：python tools/fonts/audit_glyphs.py
需要：pip install fonttools
"""
import glob
import os
import re
import sys

try:
    from fontTools.ttLib import TTFont
except ImportError:
    print("缺少 fonttools：请先执行  python -m pip install fonttools")
    sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
UI_DIR = os.path.join(ROOT, "components")
TTF = os.path.join(ROOT, "components", "middleware", "fs_storage", "spiffs_image",
                   "fonts", "notosc_reg.ttf")

# 与固件字符串一致：常规 C 字符串字面量（不做转义展开，够用）
STR_PAT = re.compile(r'"((?:[^"\\]|\\.)*)"')

# 与 make_fonts.py 保持一致的排除项：内嵌网页字符串由浏览器字体渲染，
# 不参与"设备屏幕缺字"审计（其中的 emoji/✔✘ 本就不该出现在 CJK TTF 里）。
EXCLUDE_FILES = { "web_console_page.c" }


def collect_source_chars():
    files = []
    for ext in ("*.c", "*.h"):
        files += glob.glob(os.path.join(UI_DIR, "**", ext), recursive=True)
    chars = {}
    for path in files:
        if os.path.basename(path) in EXCLUDE_FILES:
            continue
        with open(path, encoding="utf-8") as f:
            src = f.read()
        for m in STR_PAT.finditer(src):
            for ch in m.group(1):
                if ch == "\\":
                    continue
                chars[ch] = chars.get(ch, 0) + 1
    return chars


def main():
    if not os.path.exists(TTF):
        print("找不到字体文件：%s" % TTF)
        return 2

    chars = collect_source_chars()
    font = TTFont(TTF)
    cmap = font.getBestCmap()

    # 探针：U+26A0（⚠ 警告三角）是否在字体里（仅提示，不阻断退出码）。
    # v0.6.2：UI 文案已全部改用全角"！"，这里保留探针便于日后核对——
    # 若哪天又在文案里写了 ⚠，它能第一时间提醒"此字形可能缺失，会显示方框"。
    if 0x26A0 in cmap:
        print("提示：字体包含 U+26A0（⚠）")
    else:
        print("提示：字体不含 U+26A0（⚠），UI 文案若出现 ⚠ 会显示为方框")

    missing = sorted([c for c in chars if ord(c) not in cmap and ord(c) > 0x20])
    total = len(chars)
    print("源码唯一字符数（components）: %d" % total)
    print("字体 cmap 字符数  : %d" % len(cmap))
    print("缺失（会显示为方框）: %d" % len(missing))

    if missing:
        print("\n缺失字符明细（Unicode / 字符 / 在源码中出现次数）：")
        for ch in missing:
            print("  U+%04X  %s  x%d" % (ord(ch), ch, chars[ch]))
    else:
        print("\n未发现缺失字符 ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())