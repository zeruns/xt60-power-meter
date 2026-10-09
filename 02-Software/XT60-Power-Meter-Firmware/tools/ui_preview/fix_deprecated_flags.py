# -*- coding: utf-8 -*-
"""一次性把 LVGL 9.6 已废弃的 lv_obj_(add|clear|remove)_flag 用法改为专用 setter。

废弃项（-Wdeprecated-declarations）：
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE)  -> lv_obj_set_clickable(o, false)
    lv_obj_add_flag  (o, LV_OBJ_FLAG_CLICKABLE)  -> lv_obj_set_clickable(o, true)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE) -> lv_obj_set_scrollable(o, false)
    lv_obj_add_flag  (o, LV_OBJ_FLAG_SCROLLABLE) -> lv_obj_set_scrollable(o, true)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN)     -> lv_obj_set_hidden(o, false)
    lv_obj_add_flag  (o, LV_OBJ_FLAG_HIDDEN)     -> lv_obj_set_hidden(o, true)

其它 flag 组合（如 SCROLLABLE|CLICKABLE）不处理，保持原样。
仅作用于本工程自己的 UI 源码目录，不动 vendored LVGL。
"""
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", "components", "services", "ui", "src"))

PAT = re.compile(
    r"lv_obj_(add|clear|remove)_flag\s*\(\s*([^,]+?)\s*,\s*LV_OBJ_FLAG_(CLICKABLE|SCROLLABLE|HIDDEN)\s*\)")
SETTER = {"CLICKABLE": "lv_obj_set_clickable",
          "SCROLLABLE": "lv_obj_set_scrollable",
          "HIDDEN": "lv_obj_set_hidden"}

total = 0
for dirpath, _dirs, files in os.walk(ROOT):
    for fn in files:
        if not fn.endswith((".c", ".h")):
            continue
        p = os.path.join(dirpath, fn)
        with open(p, "r", encoding="utf-8") as f:
            src = f.read()
        n = [0]

        def repl(m):
            verb, obj, flag = m.group(1), m.group(2), m.group(3)
            val = "true" if verb == "add" else "false"
            n[0] += 1
            return f"{SETTER[flag]}({obj}, {val})"

        out = PAT.sub(repl, src)
        if n[0]:
            with open(p, "w", encoding="utf-8") as f:
                f.write(out)
            print(f"{os.path.relpath(p, ROOT)}: {n[0]} 处")
            total += n[0]
print(f"共计 {total} 处")
