/**
 * @file FreeRTOS.h（UI 模拟器桩，v0.6.0 新增）
 * @brief 模拟器没有 FreeRTOS：这里只提供"编译界面代码所需的最小子集"
 *
 * 为什么需要：screen_settings.c 的"忘记网络 / 连接路由器"要起一次性任务
 * （真机上用 xTaskCreatePinnedToCore + vTaskDelete，见该文件的卡死修复说明），
 * 而 PC 端模拟器没有 RTOS。桩把它退化成"同步执行一次"（见 task.h 的实现），
 * 这样界面流程在模拟器里依然完整可走查，只是不再有真正的并发。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void *TaskHandle_t;  /**< 任务句柄（模拟器里不透明指针） */
typedef uint32_t TickType_t; /**< 时钟节拍（毫秒） */
typedef int BaseType_t;      /**< FreeRTOS 风格返回值 */
typedef unsigned int UBaseType_t;

#define pdPASS 1  /**< 成功 */
#define pdTRUE 1  /**< 真 */
#define pdFALSE 0 /**< 假 */
#define pdFAIL 0  /**< 失败 */
#define portMAX_DELAY 0xFFFFFFFFu /**< 无限等待（模拟器不用） */
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms)) /**< 毫秒 → 节拍（模拟器 1 节拍 = 1ms） */
#define configTICK_RATE_HZ 1000
