# -*- coding: utf-8 -*-
"""
make_fonts.py — 生成 SPIFFS 用中文字体子集（Regular + Bold）

数据源：Noto Sans SC（SIL OFL 1.1 开源授权）可变 TTF（google/fonts 仓库）。
流程：下载 VF → 按字重实例化（400/700）→ 子集化（ASCII + GB2312 一级字库
3755 字 + 全角标点/单位符号）→ 输出到 fs_storage/spiffs_image/fonts/。

为什么不用 simhei.ttf / msyhbd.ttc：二者为微软版权字体，随固件分发有授权风险。
为什么自己子集：全量 Noto SC 单字重约 10MB+，子集后约 1~2MB，8MB SPIFFS 分区
可轻松容纳两个字重，且覆盖几乎所有常用汉字，不会再出方框。

用法：
    python make_fonts.py
依赖：
    pip install fonttools requests
"""
import glob
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE_DIR = os.path.join(HERE, ".cache")
OUT_DIR = os.path.abspath(os.path.join(HERE, "..", "..", "components",
                                       "middleware", "fs_storage", "spiffs_image", "fonts"))

VF_URLS = [
    "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf",
    "https://cdn.jsdelivr.net/gh/google/fonts@main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf",
    "https://gitee.com/mirrors_google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf",
]
VF_CACHE = os.path.join(CACHE_DIR, "NotoSansSC-VF.ttf")

# (输出文件名, 字重)
# ⚠ "reg" 实际取 500（Medium）而不是 400（Regular）：本机是 2.4 寸高亮 IPS，
#   12~16px 的 Regular 笔画偏细，在强背光下看起来发虚；Medium 笔画略粗，
#   同样的 AA 渲染下观感明显更清晰（见 tools/ui_preview 的对比图）。
WEIGHTS = [("notosc_reg.ttf", 500), ("notosc_bold.ttf", 700)]


def collect_unicodes():
    """返回需要保留的 Unicode 码点集合。"""
    codes = set()
    # ASCII 可打印字符
    codes.update(range(0x20, 0x7F))
    # CJK 标点符号区（、。〒 等）
    codes.update(range(0x3000, 0x303F + 1))
    # 全角 ASCII / 标点（含全角冒号 U+FF1A 等）
    codes.update(range(0xFF01, 0xFF5E + 1))
    # GB2312 一级字库（一级汉字，第一字节 B0~D7，第二字节 A1~FE，约 3755 字）
    for b1 in range(0xB0, 0xD7 + 1):
        for b2 in range(0xA1, 0xFE + 1):
            try:
                ch = bytes((b1, b2)).decode("gb2312")
                codes.add(ord(ch))
            except (UnicodeDecodeError, ValueError):
                pass
    # 常用符号/单位（UI 文案与仪表显示会用到）
    extras = (
        "℃°±×÷≈≤≥∞π√μΩΦφ·"
        "←→↑↓↔▲△▼▽●○■□◆◇◀▶▷◁"
        "①②③④⑤⑥⑦⑧⑨⑩"
        "✓✔✗★☆♪"
        "“”‘’—…°"
    )
    codes.update(ord(c) for c in extras)
    # UI 源码里出现的所有字符（结构性保障：文案用到的字一定会被包含，
    # 不再依赖"手工补字"；配套 tools/fonts/audit_glyphs.py 做缺字校验）
    codes.update(_scan_ui_source_chars())
    return codes


# 【v0.6.2 需求 3】扫描范围由 components/services/ui/src 扩大到整个 components：
# 之前只扫 UI 目录，导致"字库缺字"——任何**上游组件**里定义、最终却显示到屏幕上的
# 字符串都可能被漏掉（例如 power_measure/settings 的告警名、wifi_manager 的提示语）。
# 扩到 components 根后，凡是 .c/.h 里的字符串字面量全部纳入子集，从源头消除缺字。
UI_SRC = os.path.abspath(os.path.join(HERE, "..", "..", "components"))
_STR_PAT = re.compile(r'"((?:[^"\\]|\\.)*)"')

# 不参与设备字库子集的文件：内嵌网页（HTML/JS 源码里的大段字符串）最终由
# **浏览器字体**渲染，而不是 LVGL 的 CJK TTF，因此其中的 emoji（🔒/🔓）、
# ✔/✘ 等符号既不需要也无法子集进本 TTF（源字体 Noto Sans SC 也没有这些字形）。
# 排除后，字体子集与缺字审计的口径都只覆盖"真正会画到设备屏幕上"的文案。
_SCAN_EXCLUDE_FILES = { "web_console_page.c" }


def _scan_ui_source_chars():
    """扫描 components 下 C 源码的字符串字面量，返回其中出现的字符码点集合。"""
    codes = set()
    for ext in ("*.c", "*.h"):
        for path in glob.glob(os.path.join(UI_SRC, "**", ext), recursive=True):
            if os.path.basename(path) in _SCAN_EXCLUDE_FILES:
                continue
            try:
                with open(path, encoding="utf-8") as f:
                    src = f.read()
            except OSError:
                continue
            for m in _STR_PAT.finditer(src):
                for ch in m.group(1):
                    if ch != "\\":
                        codes.add(ord(ch))
    return codes


def download_vf():
    if os.path.exists(VF_CACHE) and os.path.getsize(VF_CACHE) > 1_000_000:
        print(f"使用缓存: {VF_CACHE}")
        return VF_CACHE
    os.makedirs(CACHE_DIR, exist_ok=True)
    last_err = None
    for url in VF_URLS:
        try:
            print(f"下载: {url}")
            req = urllib.request.Request(url, headers={"User-Agent": "make_fonts/1.0"})
            with urllib.request.urlopen(req, timeout=120) as resp, open(VF_CACHE, "wb") as f:
                data = resp.read()
                f.write(data)
            if os.path.getsize(VF_CACHE) > 1_000_000:
                print(f"下载完成: {os.path.getsize(VF_CACHE) / 1e6:.1f} MB")
                return VF_CACHE
        except Exception as e:  # noqa: BLE001
            print(f"  失败: {e}")
            last_err = e
    raise SystemExit(f"可变字体下载失败：{last_err}")


def main():
    try:
        from fontTools import subset
        from fontTools.varLib import instancer
        from fontTools.ttLib import TTFont
    except ImportError:
        raise SystemExit("缺少 fontTools，请先执行：pip install fonttools")

    os.makedirs(OUT_DIR, exist_ok=True)
    vf_path = download_vf()
    unicodes = collect_unicodes()
    print(f"子集码点数量: {len(unicodes)}")

    for filename, weight in WEIGHTS:
        print(f"\n生成 {filename}（wght={weight}）...")
        font = TTFont(vf_path, recalcBBoxes=False, lazy=True)
        # 实例化可变字重
        inst = instancer.instantiateVariableFont(font, {"wght": weight}, inplace=True)

        ss = subset.Subsetter(options=subset.Options(
            layout_features=[],     # 不保留 GSUB 连字特性（CJK 不需要）
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

        out_path = os.path.join(OUT_DIR, filename)
        inst.save(out_path)
        size_kb = os.path.getsize(out_path) / 1024
        glyphs = len(inst.getGlyphOrder())
        print(f"  输出: {out_path}")
        print(f"  大小: {size_kb:.0f} KB，字形数: {glyphs}")

    print("\n完成。重新构建固件后字体镜像随 idf.py flash 烧入 storage 分区。")


if __name__ == "__main__":
    main()
