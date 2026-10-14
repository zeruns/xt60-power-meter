# -*- coding: utf-8 -*-
"""audit_glyphs.py — UI 文案与字体子集的缺字审计（字库缺失闭环校验）

做两件事：
  1. 扫描 components/services/ui/src 下所有 C 源码里的字符串字面量，
     收集其中出现的全部字符（去重）；
  2. 与工程内实际烧录的 Noto Sans SC 子集 TTF（notosc_reg.ttf）的 cmap 比对，
     列出"源码要用、但字体里没有"的字符（即真机上会显示成方框的字）。

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
UI_DIR = os.path.join(ROOT, "components", "services", "ui", "src")
TTF = os.path.join(ROOT, "components", "middleware", "fs_storage", "spiffs_image",
                   "fonts", "notosc_reg.ttf")

# 与固件字符串一致：常规 C 字符串字面量（不做转义展开，够用）
STR_PAT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def collect_source_chars():
    files = []
    for ext in ("*.c", "*.h"):
        files += glob.glob(os.path.join(UI_DIR, "**", ext), recursive=True)
    chars = {}
    for path in files:
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

    missing = sorted([c for c in chars if ord(c) not in cmap and ord(c) > 0x20])
    total = len(chars)
    print("UI 源码唯一字符数: %d" % total)
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