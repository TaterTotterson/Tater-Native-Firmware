#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "xmos_update.h"

struct xmos_test_semaphore {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    bool given;
};

struct xmos_test_task {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t stopped;
    TaskFunction_t function;
    void *arg;
    bool deleting;
};

static bool fail_semaphore;
static bool fail_task;
static atomic_int live_semaphores;
static atomic_int live_tasks;
static atomic_int callbacks;
static _Thread_local TaskHandle_t current_task;

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    if (fail_semaphore) {
        return NULL;
    }
    SemaphoreHandle_t semaphore = calloc(1, sizeof(*semaphore));
    assert(semaphore);
    assert(pthread_mutex_init(&semaphore->mutex, NULL) == 0);
    assert(pthread_cond_init(&semaphore->ready, NULL) == 0);
    atomic_fetch_add(&live_semaphores, 1);
    return semaphore;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(pthread_mutex_lock(&semaphore->mutex) == 0);
    semaphore->given = true;
    assert(pthread_cond_signal(&semaphore->ready) == 0);
    assert(pthread_mutex_unlock(&semaphore->mutex) == 0);
    // Let the caller wake while the worker is still inside xSemaphoreGive.
    // Cleanup must join the worker before destroying this semaphore/context.
    struct timespec delay = {.tv_nsec = 1000000};
    nanosleep(&delay, NULL);
    assert(pthread_mutex_lock(&semaphore->mutex) == 0);
    assert(pthread_mutex_unlock(&semaphore->mutex) == 0);
    return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(timeout == portMAX_DELAY);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    assert(pthread_mutex_lock(&semaphore->mutex) == 0);
    while (!semaphore->given) {
        assert(pthread_cond_timedwait(&semaphore->ready, &semaphore->mutex, &deadline) == 0);
    }
    semaphore->given = false;
    assert(pthread_mutex_unlock(&semaphore->mutex) == 0);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    assert(pthread_cond_destroy(&semaphore->ready) == 0);
    assert(pthread_mutex_destroy(&semaphore->mutex) == 0);
    free(semaphore);
    atomic_fetch_sub(&live_semaphores, 1);
}

static void *task_entry(void *arg)
{
    current_task = arg;
    current_task->function(current_task->arg);
    abort(); // FreeRTOS tasks must not return.
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task, const char *name,
    uint32_t stack_bytes, void *arg, UBaseType_t priority, TaskHandle_t *handle,
    BaseType_t core, UBaseType_t caps)
{
    (void)name;
    (void)priority;
    (void)core;
    assert(stack_bytes >= 16384);
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_task) {
        return pdFAIL;
    }
    TaskHandle_t worker = calloc(1, sizeof(*worker));
    assert(worker);
    worker->function = task;
    worker->arg = arg;
    assert(pthread_mutex_init(&worker->mutex, NULL) == 0);
    assert(pthread_cond_init(&worker->stopped, NULL) == 0);
    atomic_fetch_add(&live_tasks, 1);
    *handle = worker;
    assert(pthread_create(&worker->thread, NULL, task_entry, worker) == 0);
    return pdPASS;
}

void vTaskSuspend(TaskHandle_t task)
{
    assert(task == NULL && current_task);
    assert(pthread_mutex_lock(&current_task->mutex) == 0);
    while (!current_task->deleting) {
        assert(pthread_cond_wait(&current_task->stopped, &current_task->mutex) == 0);
    }
    assert(pthread_mutex_unlock(&current_task->mutex) == 0);
    pthread_exit(NULL);
}

void vTaskDeleteWithCaps(TaskHandle_t task)
{
    assert(task && current_task == NULL); // Cleanup belongs to the caller.
    assert(pthread_mutex_lock(&task->mutex) == 0);
    task->deleting = true;
    assert(pthread_cond_signal(&task->stopped) == 0);
    assert(pthread_mutex_unlock(&task->mutex) == 0);
    assert(pthread_join(task->thread, NULL) == 0);
    assert(pthread_cond_destroy(&task->stopped) == 0);
    assert(pthread_mutex_destroy(&task->mutex) == 0);
    free(task);
    atomic_fetch_sub(&live_tasks, 1);
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    assert(task == NULL && current_task);
    return 4096;
}

static esp_err_t update_ok(void)
{
    assert(current_task); // The update must never execute on the caller stack.
    atomic_fetch_add(&callbacks, 1);
    return ESP_OK;
}

static esp_err_t update_bad_image(void)
{
    assert(current_task);
    atomic_fetch_add(&callbacks, 1);
    return ESP_ERR_INVALID_CRC;
}

static void *concurrent_caller(void *arg)
{
    bool *bad_image = arg;
    esp_err_t result = tater_xmos_update_run(*bad_image ? update_bad_image : update_ok);
    assert(result == (*bad_image ? ESP_ERR_INVALID_CRC : ESP_OK));
    return NULL;
}

static void assert_no_resources(void)
{
    assert(atomic_load(&live_tasks) == 0);
    assert(atomic_load(&live_semaphores) == 0);
}

int main(void)
{
    assert(tater_xmos_update_run(NULL) == ESP_ERR_INVALID_ARG);
    assert_no_resources();
    fail_semaphore = true;
    assert(tater_xmos_update_run(update_ok) == ESP_ERR_NO_MEM);
    fail_semaphore = false;
    assert_no_resources();
    fail_task = true;
    assert(tater_xmos_update_run(update_ok) == ESP_ERR_NO_MEM);
    fail_task = false;
    assert_no_resources();
    assert(atomic_load(&callbacks) == 0);

    for (int i = 0; i < 50; i++) {
        assert(tater_xmos_update_run(update_ok) == ESP_OK);
        assert(tater_xmos_update_run(update_bad_image) == ESP_ERR_INVALID_CRC);
        assert_no_resources();
    }

    pthread_t callers[2];
    bool bad_image[2] = {false, true};
    for (int i = 0; i < 2; i++) {
        assert(pthread_create(&callers[i], NULL, concurrent_caller, &bad_image[i]) == 0);
    }
    for (int i = 0; i < 2; i++) {
        assert(pthread_join(callers[i], NULL) == 0);
    }
    assert(atomic_load(&callbacks) == 102);
    assert_no_resources();
    puts("XMOS update host tests passed: failures, results, repeated/concurrent calls, worker cleanup");
    return 0;
}
