#include "xmos_update.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define XMOS_UPDATE_TASK_STACK_BYTES 16384
#define XMOS_UPDATE_TASK_PRIORITY 4
#define XMOS_UPDATE_TASK_CORE 0

static const char *TAG = "tater_xmos_update";

typedef struct {
    esp_err_t (*update)(void);
    SemaphoreHandle_t done;
    esp_err_t result;
} xmos_update_context_t;

// Based on lloydc99's Voice PE fix in PR #24. Decompression and the firmware
// writes need more stack than main has; keep the entire operation in PSRAM.
static void xmos_update_task(void *arg)
{
    xmos_update_context_t *context = arg;
    ESP_LOGW(TAG, "xmos firmware update running in dedicated task stack=%d", XMOS_UPDATE_TASK_STACK_BYTES);
    context->result = context->update();
    ESP_LOGI(TAG, "xmos firmware update finished result=%s stack_min_free=%u",
             esp_err_to_name(context->result), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(context->done);
    // The waiting caller owns cleanup. Do not self-delete: WithCaps would need
    // another internal-RAM task to reclaim this task's stack and TCB.
    for (;;) {
        vTaskSuspend(NULL);
    }
}

esp_err_t tater_xmos_update_run(esp_err_t (*update)(void))
{
    if (!update) {
        return ESP_ERR_INVALID_ARG;
    }

    xmos_update_context_t context = {
        .update = update,
        .done = xSemaphoreCreateBinary(),
        .result = ESP_FAIL,
    };
    if (!context.done) {
        ESP_LOGE(TAG, "xmos update completion semaphore alloc failed");
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t handle = NULL;
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        xmos_update_task,
        "xmos_update",
        XMOS_UPDATE_TASK_STACK_BYTES,
        &context,
        XMOS_UPDATE_TASK_PRIORITY,
        &handle,
        XMOS_UPDATE_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (created != pdPASS) {
        vSemaphoreDelete(context.done);
        ESP_LOGE(TAG, "xmos update PSRAM task create failed stack=%d", XMOS_UPDATE_TASK_STACK_BYTES);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(context.done, portMAX_DELAY);
    // This API suspends the worker and waits for it to stop before deleting
    // its task and freeing the stack/TCB, including when called on another core.
    vTaskDeleteWithCaps(handle);
    vSemaphoreDelete(context.done);
    return context.result;
}
