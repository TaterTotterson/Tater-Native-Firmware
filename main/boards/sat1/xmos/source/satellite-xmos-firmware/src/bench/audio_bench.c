#include "audio_bench.h"

#if appconfSAT1_AUDIO_BENCH
#include <xcore/hwtimer.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "rtos_osal.h"
#include "usb/usb_cdc.h"

typedef struct {
    uint32_t entered, completed, start, last, maximum, slow, active;
} stage_stats_t;

static stage_stats_t stages[SAT1_BENCH_STAGE_COUNT];
static uint32_t usb_dropped;
/* Only the tile-1 input task touches these capture fields. The pointers are
 * consumed before that same task can receive the following microphone frame.
 */
static const int32_t *raw_samples;
static uint32_t frame_sequence;

/* Fixed ownership per tile. Capture/AEC overlap on tile 1; three postprocessing
 * stages overlap on tile 0. Queue pressure blocks the input task instead of
 * growing the heap or dropping an already captured frame.
 */
#if ON_TILE(0)
#define BENCH_POOL_SLOTS 3
#else
#define BENCH_POOL_SLOTS 2
#endif
static uint64_t frame_pool[BENCH_POOL_SLOTS][SAT1_BENCH_PIPELINE_FRAME_BYTES / 8];
static QueueHandle_t free_frames;

void sat1_bench_pool_init(void)
{
    configASSERT(free_frames == NULL);
    free_frames = xQueueCreate(BENCH_POOL_SLOTS, sizeof(void *));
    configASSERT(free_frames != NULL);
    for (unsigned i = 0; i < BENCH_POOL_SLOTS; ++i) {
        void *frame = frame_pool[i];
        BaseType_t result = xQueueSend(free_frames, &frame, 0);
        configASSERT(result == pdPASS);
    }
}

void *sat1_bench_frame_acquire(size_t size)
{
    configASSERT(size <= SAT1_BENCH_PIPELINE_FRAME_BYTES && free_frames != NULL);
    void *frame = NULL;
    sat1_bench_begin(SAT1_BENCH_FRAME_POOL_WAIT);
    BaseType_t result = xQueueReceive(free_frames, &frame, portMAX_DELAY);
    configASSERT(result == pdPASS);
    sat1_bench_end(SAT1_BENCH_FRAME_POOL_WAIT);
    return frame;
}

void sat1_bench_frame_release(void *frame)
{
    unsigned index;
    for (index = 0; index < BENCH_POOL_SLOTS; ++index) {
        if (frame == (void *)frame_pool[index]) break;
    }
    configASSERT(index < BENCH_POOL_SLOTS);
    BaseType_t result = xQueueSend(free_frames, &frame, 0);
    configASSERT(result == pdPASS);
}

void sat1_bench_begin(sat1_bench_stage_t stage)
{
    uint32_t now = get_reference_time();
    taskENTER_CRITICAL();
    stages[stage].entered++;
    stages[stage].start = now;
    stages[stage].active = 1;
    taskEXIT_CRITICAL();
}

void sat1_bench_end(sat1_bench_stage_t stage)
{
    uint32_t now = get_reference_time();
    taskENTER_CRITICAL();
    stage_stats_t *s = &stages[stage];
    s->last = now - s->start;
    if (s->last > s->maximum) s->maximum = s->last;
    if (s->last > SAT1_BENCH_TIMER_HZ * 15u / 1000u) s->slow++;
    s->completed++;
    s->active = 0;
    taskEXIT_CRITICAL();
}

void sat1_bench_arm_capture(void) { raw_samples = NULL; }

void sat1_bench_note_raw(const int32_t *samples, size_t count)
{
    raw_samples = count == SAT1_BENCH_FRAME_SAMPLES ? samples : NULL;
}

void sat1_bench_snapshot(sat1_bench_frame_t *frame,
                         const int32_t *left, const int32_t *right)
{
    frame->sequence = ++frame_sequence;
    frame->capture_tick = get_reference_time();
    frame->flags = (SAT1_BENCH_FORMAT_VERSION << 2) |
                   (appconfSAT1_BENCH_PROFILE << 4) |
                   ((uint32_t)APP_VERSION_COUNTER << 8);
    if (raw_samples != NULL) {
        memcpy(frame->raw, raw_samples, sizeof(frame->raw));
        frame->flags |= SAT1_BENCH_RAW_VALID;
    } else {
        memset(frame->raw, 0, sizeof(frame->raw));
    }
    memcpy(frame->reference[0], left, sizeof(frame->reference[0]));
    memcpy(frame->reference[1], right, sizeof(frame->reference[1]));
    frame->flags |= SAT1_BENCH_REFERENCE_VALID;
}

void sat1_bench_usb_drop(void)
{
    taskENTER_CRITICAL();
    usb_dropped++;
    taskEXIT_CRITICAL();
}

/* Reporting runs outside the DSP tasks. Even a disconnected or blocked CDC
 * reader cannot hold an audio task inside a logging call. Tick ages wrap at
 * 2^32 / 100 MHz; counters remain useful across longer stalls.
 */
static void bench_monitor(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t dropped;
        taskENTER_CRITICAL();
        dropped = usb_dropped;
        taskEXIT_CRITICAL();
        cdc_printf("SAT1_BENCH t=%u heap=%u minheap=%u usb_drop=%lu\n",
                   THIS_XCORE_TILE, (unsigned)xPortGetFreeHeapSize(),
                   (unsigned)xPortGetMinimumEverFreeHeapSize(),
                   (unsigned long)dropped);
        for (unsigned i = 0; i < SAT1_BENCH_STAGE_COUNT; ++i) {
            stage_stats_t s;
            uint32_t now;
            taskENTER_CRITICAL();
            s = stages[i];
            now = get_reference_time();
            taskEXIT_CRITICAL();
            if (s.entered == 0) continue;
            cdc_printf("SAT1_BENCH t=%u s=%u in=%lu out=%lu active=%lu age=%lu last=%lu max=%lu slow=%lu\n",
                       THIS_XCORE_TILE, i, (unsigned long)s.entered,
                       (unsigned long)s.completed, (unsigned long)s.active,
                       (unsigned long)(s.active ? now - s.start : 0),
                       (unsigned long)s.last, (unsigned long)s.maximum,
                       (unsigned long)s.slow);
        }
    }
}

void sat1_bench_monitor_start(void)
{
    BaseType_t ok = xTaskCreate(bench_monitor, "audio bench",
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(bench_monitor),
        NULL, tskIDLE_PRIORITY + 1, NULL);
    configASSERT(ok == pdPASS);
}
#endif
