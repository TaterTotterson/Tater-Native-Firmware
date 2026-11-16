#!/usr/bin/env python3
"""Exercise the production cleanup worker with all late allocations disabled."""

from pathlib import Path
import shutil
import subprocess
import tempfile

MAIN = Path(__file__).resolve().parents[1]
PLATFORM = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#define configSUPPORT_STATIC_ALLOCATION 1
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdTRUE 1
#define portMAX_DELAY 99
#define ESP_LOGE(...) ((void)0)
typedef int esp_err_t;
typedef uint8_t StackType_t;
typedef struct { int unused; } StaticQueue_t;
typedef struct { int unused; } StaticTask_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
static void (*worker)(void *);
static void *queue_items[16];
static unsigned queue_count, deleted, allocations, creates, frees, delays;
static int send_failures;
static bool fail_alloc, fail_worker, forbid_alloc;
static uint8_t reserved_stack[3072];
static TaskHandle_t current = (void *)(uintptr_t)123;
static jmp_buf jump;
static void *heap_caps_malloc(size_t size, unsigned caps) {
    assert(!forbid_alloc && size == sizeof(reserved_stack));
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++allocations;
    return fail_alloc ? NULL : reserved_stack;
}
static void heap_caps_free(void *p) { assert(p == reserved_stack); ++frees; }
static QueueHandle_t xQueueCreateStatic(unsigned count, unsigned size, uint8_t *storage, StaticQueue_t *buffer) {
    assert(count == 16 && size == sizeof(TaskHandle_t) && storage && buffer);
    return buffer;
}
static TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void *), const char *name,
        unsigned size, void *arg, unsigned priority, StackType_t *stack, StaticTask_t *buffer, int core) {
    assert(!forbid_alloc && name && size == sizeof(reserved_stack) && !arg);
    assert(priority > 0 && stack == reserved_stack && core == 0);
    ++creates;
    worker = fn;
    return fail_worker ? NULL : buffer;
}
static int xQueueSend(QueueHandle_t queue, const void *item, int timeout) {
    assert(queue && timeout == portMAX_DELAY);
    if (send_failures > 0) { --send_failures; return 0; }
    assert(queue_count < 16);
    queue_items[queue_count++] = *(TaskHandle_t *)item;
    return pdTRUE;
}
static int xQueueReceive(QueueHandle_t queue, void *out, int timeout) {
    assert(queue && timeout == portMAX_DELAY);
    if (!queue_count) longjmp(jump, 3);
    *(TaskHandle_t *)out = queue_items[--queue_count];
    return pdTRUE;
}
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
static void vTaskDelay(int ticks) { assert(ticks == 1); ++delays; }
static void vTaskSuspend(TaskHandle_t task) { assert(!task); longjmp(jump, 1); }
static void vTaskDelete(TaskHandle_t task) { assert(!task); longjmp(jump, 2); }
static void vTaskDeleteWithCaps(TaskHandle_t task) {
    // A NULL/current handle would trigger IDF's allocating self-delete helper.
    assert(forbid_alloc && task && task != current);
    ++deleted;
}
'''

TESTS = r'''
#include <stdio.h>
int main(void) {
    // Before initialization, parking is safer than the SDK's allocating helper.
    if (setjmp(jump) == 0) tater_task_delete_current(true);
    assert(!queue_count && !allocations);
    fail_alloc = true;
    assert(tater_task_cleanup_init() == ESP_ERR_NO_MEM);
    fail_alloc = false;
    fail_worker = true;
    assert(tater_task_cleanup_init() == ESP_ERR_NO_MEM && frees == 1);
    fail_worker = false;
    assert(tater_task_cleanup_init() == ESP_OK);
    unsigned allocated = allocations, created = creates;
    forbid_alloc = true;
    assert(tater_task_cleanup_init() == ESP_OK);
    assert(allocations == allocated && creates == created);
    for (unsigned batch = 0; batch < 20; ++batch) {
        for (unsigned i = 0; i < 16; ++i) {
            current = (void *)(uintptr_t)(100 + batch * 16 + i);
            if (setjmp(jump) == 0) tater_task_delete_current(true);
        }
        assert(queue_count == 16);
        current = (void *)(uintptr_t)1; // The reusable cleanup worker.
        if (setjmp(jump) == 0) worker(NULL);
        assert(!queue_count && deleted == (batch + 1) * 16);
    }
    send_failures = 2;
    current = (void *)(uintptr_t)999;
    if (setjmp(jump) == 0) tater_task_delete_current(true);
    assert(queue_count == 1 && delays == 2);
    current = (void *)(uintptr_t)1;
    if (setjmp(jump) == 0) worker(NULL);
    assert(deleted == 321);
    int result = setjmp(jump);
    if (!result) tater_task_delete_current(false);
    assert(result == 2 && !queue_count);
    assert(allocations == allocated && creates == created);
    puts("PASS: reserved cleanup handles bursts/backpressure without late allocations or WithCaps self-deletion");
}
'''

with tempfile.TemporaryDirectory(prefix="tater-task-cleanup-") as directory:
    build = Path(directory)
    (build / "freertos").mkdir()
    (build / "test_platform.h").write_text(PLATFORM)
    for header in ("esp_err.h", "esp_heap_caps.h", "esp_log.h", "freertos/FreeRTOS.h",
                   "freertos/idf_additions.h", "freertos/queue.h", "freertos/task.h"):
        (build / header).write_text('#include "test_platform.h"\n')
    source = build / "cleanup_test.c"
    source.write_text((MAIN / "task_cleanup.c").read_text() + TESTS)
    compiler = shutil.which("cc")
    if not compiler:
        raise SystemExit("A C compiler is required")
    binary = build / "cleanup_test"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(build), "-I", str(MAIN),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
