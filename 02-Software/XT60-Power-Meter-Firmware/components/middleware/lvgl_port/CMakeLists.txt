idf_component_register(
    SRCS "src/lvgl_port.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl lcd_nv3030b touch_cst816d esp_timer
    # 嵌入子集化中文字体（仅含本工程用到的 CJK 字符，约 25KB）
    # tiny_ttf 运行时按需渲染任意字号，替代内置 CJK 字体（字形不全、字号固定）
    EMBED_FILES "fonts/notosanssc_subset.ttf")
