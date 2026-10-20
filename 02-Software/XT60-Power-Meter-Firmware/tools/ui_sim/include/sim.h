/**
 * @file sim.h
 * @brief XT60 功率计 LVGL 模拟器内部接口
 *
 * 结构：
 *   sim_main.c        —— 渲染引擎（display/indev/虚拟时钟）+ 场景调度
 *   sim_scenes.c      —— 场景脚本（构建界面 / 模拟触摸 / 截图）
 *   lvgl_port_sim.c   —— lvgl_port 桩实现（字体/主题/锁/旋转）
 *   mocks/            —— 服务层 mock（settings/buzzer/power_measure…）
 *
 * 设计要点：
 *   · 确定性：虚拟时钟快进（默认 5ms/步），同一场景每次渲染结果一致；
 *   · 逐字节一致：显示缓冲用 RGB565_SWAPPED（与真机面板线上格式相同），
 *     截图导出时再转成 BMP 的 RGB888；
 *   · 场景即代码：每个场景是一段普通 C 代码（构建/交互/断言/截图），
 *     通过 sim_* 辅助函数驱动，等价于“可编程的验收流程”。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "data_store.h"

/* ---- 渲染引擎 ---- */

/** @brief 初始化 LVGL + 显示/输入设备 + 字体与主题（等价真机 lvgl_port_app_init） */
void sim_init(void);

/** @brief 推进虚拟时间并处理 LVGL 定时器/渲染（1 步 = 5ms tick，内部循环 ms 总时长） */
void sim_step(uint32_t ms);

/** @brief 直接按下/抬起触摸点（供场景脚本模拟交互） */
void sim_touch_down(int x, int y);
void sim_touch_move(int x, int y);
void sim_touch_up(void);

/** @brief 便捷操作：单击（按下 → 40ms → 抬起 → 等待稳定） */
void sim_click(int x, int y);

/** @brief 便捷操作：从 (x1,y1) 滑到 (x2,y2)，steps 为中间采样点数 */
void sim_swipe(int x1, int y1, int x2, int y2, int steps);

/** @brief 把当前帧缓冲导出为 BMP（out/<name>.bmp，24bpp，原尺寸 284x240） */
void sim_shot(const char *name);

/** @brief 当前逻辑分辨率（旋转后会变） */
void sim_get_resolution(int *w, int *h);

/* ---- 场景脚本 ---- */

/** @brief 场景入口（实现在 sim_scenes.c；由 sim_main 调用） */
void sim_run_scenes(void);

/* ---- 场景辅助：测量数据快照 ---- */

/** @brief 设置模拟快照（电压/电流/功率/电能/温湿度/电池/运行秒数），供 UI 刷新使用 */
void sim_set_measurement(float voltage_v, float current_a, float power_w, float energy_wh,
                         float temperature_c, float humidity_pct, uint32_t battery_mv,
                         uint32_t runtime_s);

/** @brief 取当前模拟快照（场景里调用 ui_nav_refresh 用） */
const data_store_snapshot_t *sim_get_measurement(void);

/** @brief 把快照中的电能/运行时间清零（mock 的 power_measure_reset_energy 调用） */
void sim_zero_energy(void);

/** @brief flush 计数自增（由 sim_main 的 flush 回调调用，供 lvgl_port_flush_count 使用） */
void sim_flush_count_inc(void);

/** @brief 按“当前分辨率”取屏幕中心（旋转后布局调整用） */
int sim_center_x(void);
int sim_center_y(void);