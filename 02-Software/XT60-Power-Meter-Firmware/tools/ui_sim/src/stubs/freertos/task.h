/**
 * @file task.h（UI 模拟器桩，v0.6.0 新增）
 * @brief 任务创建/删除的模拟器实现声明
 *
 * 语义：xTaskCreatePinnedToCore() 在本模拟器里**同步执行一次任务函数**
 * （等价于"直接调用"），任务函数末尾的 vTaskDelete(NULL) 是空操作。
 * 界面因此能走到"操作完成 → 弹结果框"的完整分支，便于截图走查。
 */
#pragma once

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);

/**
 * @brief 创建任务并绑定核（模拟器：立即同步执行任务函数一次）
 * @return 恒返回 pdPASS（模拟器不会因为资源不足失败）
 */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                                   void *arg, UBaseType_t prio, TaskHandle_t *out_handle,
                                   int core_id);

/**
 * @brief 删除任务（模拟器：空操作；传 NULL 表示删自己）
 */
void vTaskDelete(TaskHandle_t task);

/** @brief 取任务句柄（模拟器：恒返回空指针，ui 层不强依赖） */
TaskHandle_t xTaskGetHandle(const char *name);
