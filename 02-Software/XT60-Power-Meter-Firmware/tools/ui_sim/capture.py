# -*- coding: utf-8 -*-
"""capture.py — 把模拟器输出的 BMP 截图转成 PNG（原尺寸 + 3 倍放大）

用法：python capture.py
输入：out/*.bmp（由 ui_sim.exe 生成）
输出：out/png/<name>.png        1x（评审/汇报用）
      out/png/<name>@3x.png    3x 最近邻放大（细节走查用）
"""
import glob
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
PNG_DIR = os.path.join(OUT, "png")


def main():
    os.makedirs(PNG_DIR, exist_ok=True)
    bmps = sorted(glob.glob(os.path.join(OUT, "*.bmp")))
    if not bmps:
        print("未找到 out/*.bmp —— 请先运行 build.ps1（或 ui_sim.exe）")
        return 1

    count = 0
    for bmp in bmps:
        name = os.path.splitext(os.path.basename(bmp))[0]
        img = Image.open(bmp).convert("RGB")
        img.save(os.path.join(PNG_DIR, name + ".png"))
        big = img.resize((img.width * 3, img.height * 3), Image.NEAREST)
        big.save(os.path.join(PNG_DIR, name + "@3x.png"))
        count += 1

    print("已转换 %d 张截图 → %s" % (count, PNG_DIR))
    return 0


if __name__ == "__main__":
    sys.exit(main())