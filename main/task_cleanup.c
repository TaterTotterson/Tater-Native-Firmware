#include "task_cleanup.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if (configSUPPORT_STATIC_ALLOCATION == 1)
#define CLEANUP_QUEUE_LENGTH 16
#define CLEANUP_STACK_BYTES 3072

static StaticQueue_t s_queue_buffer;
static uint8_t s_queue_storage[CLEANUP_QUEUE_LENGTH * sizeof(TaskHandle_t)];
static StaticTask_t s_task_buffer;
static QueueHandle_t s_queue;
static TaskHandle_t s_worker;

static void cleanup_task(void *arg)
{
    (void)arg;
    for (;;) {
        TaskHandle_t task = NULL;
        if (xQueueReceive(s_queue, &task, portMAX_DELAY) == pdTRUE && task) {
            // Deleting another task suspends it and frees its buffers directly;
            // unlike WithCaps self-deletion, this never creates a helper task.
            vTaskDeleteWithCaps(task);
        }
    }
}
#endif

esp_err_t tater_task_cleanup_init(void)
{
#if (configSUPPORT_STATIC_ALLOCATION == 1)
    if (s_worker) {
        return ESP_OK;
    }
    StackType_t *stack = heap_caps_malloc(
        CLEANUP_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!stack) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_queue) {
        s_queue = xQueueCreateStatic(
            CLEANUP_QUEUE_LENGTH, sizeof(TaskHandle_t), s_queue_storage, &s_queue_buffer
        );
    }
    if (s_queue) {
        s_worker = xTaskCreateStaticPinnedToCore(
            cleanup_task, "tater_cleanup", CLEANUP_STACK_BYTES, NULL, 5,
            stack, &s_task_buffer, 0
        );
    }
    if (!s_worker) {
        heap_caps_free(stack);
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

void tater_task_delete_current(bool task_with_caps)
{
#if (configSUPPORT_STATIC_ALLOCATION == 1)
    if (task_with_caps) {
        TaskHandle_t task = xTaskGetCurrentTaskHandle();
        if (s_worker) {
            // Backpressure uses the preallocated queue. Even a burst of task
            // completions must not fall back to allocating a cleanup helper.
            while (xQueueSend(s_queue, &task, portMAX_DELAY) != pdTRUE) {
                vTaskDelay(1);
            }
        } else {
            ESP_LOGE("tater_cleanup", "cleanup not initialized; parking completed task");
        }
        // The worker may already have suspended/deleted us on the other core.
        // If not, park until it does. Never return from a FreeRTOS task.
        for (;;) {
            vTaskSuspend(NULL);
        }
    }
#else
    (void)task_with_caps;
#endif
    vTaskDelete(NULL);
}
