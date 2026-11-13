#pragma once
#include "FreeRTOS.h"
typedef struct xmos_test_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task, const char *name,
    uint32_t stack_bytes, void *arg, UBaseType_t priority, TaskHandle_t *handle,
    BaseType_t core, UBaseType_t caps);
void vTaskDeleteWithCaps(TaskHandle_t task);
void vTaskSuspend(TaskHandle_t task);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
