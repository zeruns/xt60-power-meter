/**
 * @file lcd_nv3030b.h
 * @brief 显示驱动对外接口（240x284 IPS，QSPI 4bit）+ 背光 PWM
 *
 * 面板：TK018F3716 模组，QSPI 数据线 4bit（引脚见 board_pins.h 的 BOARD_PIN_LCD_*）；
 * 背光：BOARD_PIN_LCD_BL（IO45）经 Q3 低边驱动，由 LEDC 输出 PWM 调光。
 *
 * 职责边界：
 *   · 本驱动负责「总线 + 面板初始化 + 刷新 + 背光」；
 *   · 面板初始化序列由官方组件 espressif/esp_lcd_spd2010 提供，本驱动不自写；
 *   · LVGL 适配（显示/输入设备注册、心跳、锁）在 middleware/lvgl_port。
 *
 * 命名说明：模组规格书叫 NV3030B，厂商官方 ESP32 例程用 esp_lcd_spd2010 驱动它，
 * 本驱动沿用同一路径。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/** 面板物理玻璃尺寸（竖屏原生 240 宽 × 284 高，NV3030B 控制器 240×320 的子集） */
#define LCD_NV3030B_PHYS_W 240
#define LCD_NV3030B_PHYS_H 284

/**
 * 默认（横屏）逻辑分辨率。
 *
 * 物理玻璃 240×284（竖），默认由 MADCTL=0x68（MX|MV|BGR）硬旋转为 284×240
 * 横屏。LVGL 显示设备、清屏、绘制与触摸都使用对应旋转档位的逻辑坐标。
 * 注意：旋转档位对应的尺寸请用 lcd_nv3030b_get_resolution() 查询，
 * 本宏仅是**默认档位（ROT_90 横屏）**下的取值。
 */
#define LCD_NV3030B_H_RES 284 /**< 默认档位逻辑宽（横屏） */
#define LCD_NV3030B_V_RES 240 /**< 默认档位逻辑高（横屏） */

/**
 * 显示旋转档位（对应标准 MIPI MADCTL 组合，BGR=0x08）
 *
 * NV3030B 的 0x36 寄存器符合标准 MIPI 语义（MY=0x80/MX=0x40/MV=0x20），
 * 与 TFT_eSPI 官方 NV3030B 驱动的 rotation 定义一致：
 *   ROT_0  → 0x08 (BGR)            竖屏   240×284
 *   ROT_90 → 0x68 (MX|MV|BGR)      横屏   284×240  ← 本工程默认
 *   ROT_180→ 0xC8 (MX|MY|BGR)      竖屏倒 240×284
 *   ROT_270→ 0xA8 (MV|MY|BGR)      横屏倒 284×240
 */
typedef enum {
    LCD_NV3030B_ROT_0 = 0, /**< 竖屏（物理原生） */
    LCD_NV3030B_ROT_90,    /**< 横屏（默认） */
    LCD_NV3030B_ROT_180,   /**< 竖屏倒置 */
    LCD_NV3030B_ROT_270,   /**< 横屏倒置 */
} lcd_nv3030b_rotation_t;

/**
 * @brief 切换显示旋转档位（写 MADCTL）
 *
 * 只改面板方向，不改分辨率相关的软件状态——LVGL 侧的分辨率与触摸映射
 * 需由 lvgl_port_set_rotation() 同步（它内部会调用本函数）。
 *
 * @param rot 目标档位
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 档位非法；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t lcd_nv3030b_set_rotation(lcd_nv3030b_rotation_t rot);

/**
 * @brief 取当前旋转档位
 * @return 当前档位（未初始化时为默认 ROT_90）
 */
lcd_nv3030b_rotation_t lcd_nv3030b_get_rotation(void);

/**
 * @brief 查询某旋转档位下的逻辑分辨率
 * @param rot   档位
 * @param width  输出：逻辑宽（可为 NULL）
 * @param height 输出：逻辑高（可为 NULL）
 */
void lcd_nv3030b_get_resolution(lcd_nv3030b_rotation_t rot, int *width, int *height);

/**
 * @brief 初始化显示链路（QSPI 总线、面板、背光 PWM、开显示、现场清屏）
 *
 * 内部流程：建 QSPI 总线 → 建 panel IO → 配背光 PWM（占空比 0 = 灭）→
 * 用 esp_lcd_spd2010 创建面板 → 复位/初始化/设横屏/开显示 →
 * 刷一屏清屏色 → 点亮背光。
 *
 * 「先刷清屏色再开背光」是刻意为之：面板刚复位时显存内容未定义，
 * 若先开背光会看到「上电白屏」闪过。
 *
 * @return ESP_OK 成功；
 *         ESP_ERR_INVALID_STATE 已初始化过（重复调用）；
 *         其他值表示某个子步骤失败（失败时会把已申请的总线/IO/面板/背光全部释放）
 */
esp_err_t lcd_nv3030b_init(void);

/**
 * @brief 设置背光亮度
 * @param percent 亮度百分比 0~100（0 为完全关闭）
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 超出范围；
 *         ESP_ERR_INVALID_STATE 背光 PWM 尚未配置（init 未完成）
 */
esp_err_t lcd_nv3030b_set_backlight(uint8_t percent);

/**
 * @brief 显存写入完成回调
 *
 * ⚠ esp_lcd 对「颜色数据」的 SPI 传输是**异步**的（内部走
 *   spi_device_queue_trans），esp_lcd_panel_io_tx_color() 返回时 DMA 还没结束。
 *   上层若在 lcd_nv3030b_draw_bitmap() 返回后立刻复用绘图缓冲，就会和 DMA
 *   抢同一块内存，表现为画面撕裂、花屏甚至整屏黑。
 *   正确做法：注册本回调，等传输真正完成后再复用缓冲。
 *
 * @param user_ctx 注册时传入的上下文指针，原样回传
 * @warning 在 SPI 中断上下文执行：必须短小，禁止阻塞、禁止调用耗时函数。
 */
typedef void (*lcd_nv3030b_flush_done_cb_t)(void *user_ctx);

/**
 * @brief 注册「显存写入完成」回调
 *
 * 在 lcd_nv3030b_init() 之后、开始刷屏之前调用即可。
 *
 * @param cb 回调函数；传 NULL 表示注销
 * @param user_ctx 回调上下文（原样回传）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 面板尚未初始化
 */
esp_err_t lcd_nv3030b_register_flush_done_cb(lcd_nv3030b_flush_done_cb_t cb, void *user_ctx);

/**
 * @brief 把一块像素写入显存（供 LVGL flush 使用）
 *
 * 坐标为横屏逻辑坐标（x: 0~283，y: 0~239），区域为
 * [x_start, x_end) × [y_start, y_end)（左闭右开）。
 * color_data 为 RGB565 像素缓冲，行主序，长度 = 宽×高×2 字节。
 *
 * @param x_start 起始列（含）
 * @param y_start 起始行（含）
 * @param x_end   结束列（不含）
 * @param y_end   结束行（不含）
 * @param color_data 像素数据缓冲
 * @return ESP_OK 成功
 */
esp_err_t lcd_nv3030b_draw_bitmap(int x_start, int y_start, int x_end, int y_end,
                                  const void *color_data);

/**
 * @brief 获取底层 esp_lcd 面板句柄
 *
 * 供 ESP 官方组件 espressif/esp_lvgl_port 的 lvgl_port_add_disp() 使用
 * （它需要 panel / panel IO 句柄自行完成刷屏与传输完成回调）。
 *
 * @return 面板句柄；未初始化时返回 NULL
 */
esp_lcd_panel_handle_t lcd_nv3030b_get_panel_handle(void);

/**
 * @brief 获取底层 esp_lcd 面板 IO 句柄
 * @return 面板 IO 句柄；未初始化时返回 NULL
 */
esp_lcd_panel_io_handle_t lcd_nv3030b_get_io_handle(void);
