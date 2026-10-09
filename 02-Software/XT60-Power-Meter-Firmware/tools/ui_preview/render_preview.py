# -*- coding: utf-8 -*-
"""
render_preview.py — 284x240 UI 布局离线预览（PC 端闭环调 UI 用）

用设备同款 Noto Sans SC TTF（Regular/Medium/Bold，components/middleware/
fs_storage/spiffs_image/fonts/）做真实字形度量，按各 screen_*.c 里写死的
布局参数渲染，输出 3 倍放大 PNG，并自动报告：
  · 文本是否超出其可用宽度（溢出检测）
  · 元素顶点是否落进面板圆角遮罩区（圆角侵入检测）

不是 LVGL 的像素级复刻，但所有尺寸/坐标/字宽与固件代码一致（LVGL px 字号与
PIL truetype 像素字号视觉近似，宽度误差 1~2px），足以在烧录前发现
"数字被截掉""内容进圆角""单位被挤出去"这类问题。

用法：python render_preview.py
"""
import os
import math
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.abspath(os.path.join(HERE, "..", "..", "components", "middleware",
                                  "fs_storage", "spiffs_image", "fonts"))
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)

W, H, SCALE = 284, 240, 3
CORNER_R = 22  # 面板可见区域圆角半径（估计值，用于圆角侵入检查）

REG = os.path.join(FW, "notosc_reg.ttf")     # 实际字重 500 (Medium)
BOLD = os.path.join(FW, "notosc_bold.ttf")   # 700

_fc = {}
def font(size, bold=False):
    key = (size, bold)
    if key not in _fc:
        _fc[key] = ImageFont.truetype(BOLD if bold else REG, size)
    return _fc[key]

def text_w(s, f):
    if not s:
        return 0
    b = f.getbbox(s)
    return b[2] - b[0]

issues = []

def check_corner(name, x, y, w, h):
    """检查矩形四顶点是否进入圆角遮罩（圆角半径 CORNER_R 的圆外区域）"""
    pts = [(x, y), (x + w, y), (x, y + h), (x + w, y + h)]
    for (px, py) in pts:
        near_left = px < CORNER_R
        near_right = px > W - CORNER_R
        near_top = py < CORNER_R
        near_bot = py > H - CORNER_R
        if not ((near_left or near_right) and (near_top or near_bot)):
            continue
        ccx = CORNER_R if near_left else W - CORNER_R
        ccy = CORNER_R if near_top else H - CORNER_R
        if (px - ccx) ** 2 + (py - ccy) ** 2 < (CORNER_R - 2) ** 2:
            issues.append(f"[圆角] {name} 顶点({px},{py}) 落入圆角遮罩")

def draw_text(d, xy, s, size, color, bold=False, max_w=None, name="", align=None, width=None):
    f = font(size, bold)
    tw = text_w(s, f)
    x, y = xy
    if align == "center" and width:
        x = xy[0] + (width - tw) // 2
    elif align == "right" and width:
        x = xy[0] + width - tw
    if max_w is not None and tw > max_w:
        issues.append(f"[溢出] {name}: '{s}' 宽 {tw}px > 可用 {max_w}px")
    d.text((x, y), s, font=f, fill=color)
    return tw

def new_canvas():
    img = Image.new("RGB", (W, H), (16, 16, 20))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=CORNER_R, fill=(255, 255, 255))
    return img, d

def save(img, name):
    img.resize((W * SCALE, H * SCALE), Image.NEAREST).save(os.path.join(OUT, name))
    print("saved", os.path.join(OUT, name))

# ---- 色板（与固件一致）----
C_DARK = (0x1A, 0x1A, 0x1A)
C_GRAY = (0x6B, 0x72, 0x80)
C_BLUE = (0x1B, 0x6F, 0xD6)
C_V = (0x1E, 0x9E, 0x4F)
C_I = (0x1B, 0x6F, 0xD6)
C_P = (0xD0, 0x39, 0x2B)
C_E = (0x7D, 0x3C, 0xB5)
C_ACCENT = (0x00, 0x66, 0xCC)
C_BD = (0xE3, 0xE7, 0xEC)
WHITE = (255, 255, 255)

# ---------------- 共享状态栏 ----------------
def draw_topbar(d, runtime="00:34:38", bat="88%", bat_col=C_V):
    draw_text(d, (28, 3), runtime, 14, (0x4A, 0x4F, 0x58), name="status.runtime")
    # 右侧：WiFi 图标 + 电池图标 + 百分比（右对齐到 284-28）
    txt = f"[B] {bat}"
    tw = text_w(txt, font(14))
    draw_text(d, (W - 28 - 22, 3), "[W]", 14, (0xB9, 0xBF, 0xC7))
    draw_text(d, (W - 28 - tw, 3), txt, 14, bat_col, name="status.bat")
    check_corner("statusbar", 28, 2, W - 56, 20)

def draw_dots(d, cur=0):
    x = (W - (16 + 2 * (6 + 7))) // 2
    for k in range(3):
        wpx = 16 if k == cur else 6
        col = C_ACCENT if k == cur else (0xC2, 0xC7, 0xCF)
        d.rounded_rectangle([x, 224, x + wpx - 1, 229], radius=3, fill=col)
        x += wpx + 7

# ---------------- 测量页（四行满宽彩条）----------------
ROW_H, ROW_GAP, ROW_TOP, SAFE_X = 42, 3, 24, 16
def render_measure(v="19.97670", i="0.01886", p="0.37683", e="0.11800",
                   i_neg=False, runtime="00:34:38", bat="88%", bat_col=C_V):
    img, d = new_canvas()
    draw_topbar(d, runtime, bat, bat_col)
    rows = [("电压", v, "V", C_V, False), ("电流", i, "A", C_I, True),
            ("功率", p, "W", C_P, False), ("电能", e, "Wh", C_E, False)]
    for k, (nm, val, unit, col, arrow) in enumerate(rows):
        y = ROW_TOP + k * (ROW_H + ROW_GAP)
        d.rounded_rectangle([SAFE_X, y, W - SAFE_X - 1, y + ROW_H - 1], radius=6, fill=col)
        check_corner(f"row{k}", SAFE_X, y, W - 2 * SAFE_X, ROW_H)
        # 左：5 位小数大数字（Bold 24）
        vw = draw_text(d, (SAFE_X + 10, y + 7), val, 24, WHITE, bold=True,
                       max_w=W - 2 * SAFE_X - 20 - 60, name=f"row{k}.value")
        # 右：箭头 + 单位（右对齐到 行右内边距 10，与固件 flex 右对齐一致）
        rx = W - SAFE_X - 10
        uw = text_w(unit, font(14, True))
        draw_text(d, (rx - uw, y + 13), unit, 14, WHITE, bold=True,
                  max_w=40, name=f"row{k}.unit")
        if arrow:
            a = "◀" if i_neg else "▶"
            aw = text_w(a, font(20))
            draw_text(d, (rx - uw - 6 - aw, y + 9), a, 20, WHITE,
                      max_w=20, name=f"row{k}.arrow")
    ey = ROW_TOP + 4 * (ROW_H + ROW_GAP) + 2
    draw_text(d, (SAFE_X, ey), "32.5C  30%RH", 12, C_GRAY, name="env")
    draw_dots(d, 0)
    save(img, "measure.png")

def render_measure_neg():
    """负电流 + 大数据量（检查溢出与箭头方向）"""
    img, d = new_canvas()
    draw_topbar(d, "12:34:56", "15%", C_P)
    rows = [("电压", "123.45678", "V", C_V, False), ("电流", "-12.34567", "A", C_I, True),
            ("功率", "1523.45678", "W", C_P, False), ("电能", "12345.67890", "Wh", C_E, False)]
    for k, (nm, val, unit, col, arrow) in enumerate(rows):
        y = ROW_TOP + k * (ROW_H + ROW_GAP)
        d.rounded_rectangle([SAFE_X, y, W - SAFE_X - 1, y + ROW_H - 1], radius=6, fill=col)
        draw_text(d, (SAFE_X + 10, y + 7), val, 24, WHITE, bold=True,
                  max_w=W - 2 * SAFE_X - 20 - 60, name=f"neg{k}.value")
        rx = W - SAFE_X - 10
        uw = draw_text(d, (rx, y + 13), unit, 14, WHITE, bold=True, max_w=40)
        if arrow:
            draw_text(d, (rx - uw - 6 - 14, y + 9), "◀", 20, WHITE, max_w=20)
    draw_text(d, (SAFE_X, ROW_TOP + 4 * (ROW_H + ROW_GAP) + 2), "-5.0C  99%RH", 12, C_GRAY)
    draw_dots(d, 0)
    save(img, "measure_edge.png")

# ---------------- 波形页 ----------------
def render_wave(mode="电压+电流", win="10s", a_unit="V", b_unit="A",
                a_vals=("19.98", "19.90", "19.82"), b_vals=("0.019", "0.018", "0.017"),
                rt="实 19.97670V 0.01886A 0.37683W", av="均 19.97612V 0.01880A 0.37540W"):
    img, d = new_canvas()
    draw_topbar(d)
    draw_text(d, (0, 26), f"{mode} · {win}", 14, C_DARK, name="wave.title",
              align="center", width=W)
    cx1, cy1 = 12, 44
    cw, ch = W - 2 * SAFE_X, 138
    cx2, cy2 = cx1 + cw - 1, cy1 + ch - 1
    d.rounded_rectangle([cx1, cy1, cx2, cy2], radius=8, outline=C_BD, width=1)
    check_corner("chart", cx1, cy1, cw, ch)
    AXW = 62
    px1, py1 = cx1 + 1 + AXW + 4, cy1 + 1 + 5
    px2, py2 = cx2 - 1 - AXW - 4, cy2 - 1 - 5
    for k in range(5):
        yy = py1 + (py2 - py1) * k // 4
        d.line([px1, yy, px2, yy], fill=(0xEE, 0xF1, 0xF5))
    for k in range(6):
        xx = px1 + (px2 - px1) * k // 5
        d.line([xx, py1, xx, py2], fill=(0xEE, 0xF1, 0xF5))
    # 曲线：右对齐（最新在右），模拟双通道
    pts_a, pts_b = [], []
    N = 200
    for k in range(N + 1):
        t = k / N
        xx = px1 + t * (px2 - px1)
        ya = (py1 + py2) / 2 + math.sin(t * 18) * 14 + math.sin(t * 47) * 5
        yb = (py1 + py2) / 2 + math.cos(t * 13) * 20 + math.sin(t * 39) * 7
        pts_a.append((xx, ya)); pts_b.append((xx, yb))
    d.line(pts_b, fill=C_I, width=2)
    d.line(pts_a, fill=C_V, width=2)
    # 轴刻度带单位
    ys = [py1 - 3, (py1 + py2) // 2 - 7, py2 - 11]
    for k in range(3):
        draw_text(d, (cx1 + 4, ys[k]), f"{a_vals[k]} {a_unit}", 12, C_V,
                  max_w=AXW, name=f"axisA{k}")
        wtx = f"{b_vals[k]} {b_unit}"
        draw_text(d, (cx2 - 3, ys[k]), wtx, 12, C_I, max_w=AXW, name=f"axisB{k}",
                  align="right", width=AXW)
    # 时间轴
    draw_text(d, (px1, 184), f"-{win}", 12, C_GRAY, max_w=60, name="t.left")
    draw_text(d, (px2 - 30, 184), "0", 12, C_GRAY, max_w=30)
    draw_text(d, (SAFE_X, 196), rt, 12, C_DARK, max_w=W - 2 * SAFE_X, name="wave.rt")
    draw_text(d, (SAFE_X, 209), av, 12, C_GRAY, max_w=W - 2 * SAFE_X, name="wave.avg")
    draw_dots(d, 1)
    save(img, "wave.png")

# ---------------- 设置页 ----------------
def render_settings(dialog=False):
    img, d = new_canvas()
    draw_topbar(d)
    draw_text(d, (16, 26), "设置", 20, C_DARK, bold=True, name="settings.title")
    items = ["测量设置（标定/采样率）", "显示设置（亮度/方向）", "蜂鸣器响度",
             "WiFi 与热点", "电能清零与统计", "关于本机"]
    y = 54
    for it in items:
        d.rounded_rectangle([16, y, W - 16, y + 31], radius=6, outline=C_BD, width=1)
        draw_text(d, (24, y + 7), it, 16, C_DARK, max_w=W - 60, name="list." + it[:5])
        y += 34
    if dialog:
        # 模态对话框（居中，250 宽）
        bw, bh = 250, 130
        bx, by = (W - bw) // 2, (H - bh) // 2
        d.rounded_rectangle([bx, by, bx + bw, by + bh], radius=10, fill=(0xFF, 0xFF, 0xFF),
                            outline=C_BD, width=2)
        check_corner("dialog", bx, by, bw, bh)
        draw_text(d, (bx + 16, by + 12), "功能开发中", 16, C_DARK, bold=True,
                  max_w=bw - 32, name="dialog.title")
        draw_text(d, (bx + 16, by + 44), "该设置项将在后续版本提供", 14, (0x37, 0x41, 0x51),
                  max_w=bw - 32, name="dialog.text")
        # 底部按钮
        btw, bth = 84, 32
        bxx = bx + (bw - btw) // 2
        byy = by + bh - bth - 12
        d.rounded_rectangle([bxx, byy, bxx + btw, byy + bth - 1], radius=16, fill=C_ACCENT)
        draw_text(d, (bxx, byy + 7), "确定", 14, WHITE, align="center", width=btw,
                  name="dialog.ok")
    draw_dots(d, 2)
    save(img, "settings.png" if not dialog else "settings_dialog.png")

if __name__ == "__main__":
    render_measure()
    render_measure_neg()
    render_wave()
    render_wave(mode="功率", win="10min", a_unit="W", b_unit="",
                a_vals=("0.4", "0.3", "0.2"), b_vals=("", "", ""),
                rt="实 0.37683W", av="均 0.37540W")
    render_settings(dialog=False)
    render_settings(dialog=True)
    print()
    if issues:
        print(f"发现 {len(issues)} 处问题：")
        for s in issues:
            print("  ", s)
    else:
        print("布局检查全部通过：无文本溢出、无圆角侵入")
