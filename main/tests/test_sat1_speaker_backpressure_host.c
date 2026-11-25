/* Exercise the real speaker and generic pipelines with a blocked consumer. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define appconfAUDIO_PIPELINE_FRAME_ADVANCE 240
#define appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE (240 * RATE_FACTOR)
#define appconfAUDIO_SPK_PL_SR_FACTOR RATE_FACTOR
#define appconfAUDIO_PIPELINE_TASK_PRIORITY 5
#define configMINIMAL_STACK_SIZE 256
#define RTOS_THREAD_STACK_SIZE(function) 128
#define RTOS_OSAL_WAIT_FOREVER UINT32_MAX
#define configASSERT assert
#define xassert assert
/* These have equal widths on xcore, where generic_pipeline casts the array. */
typedef size_t configSTACK_DEPTH_TYPE;
typedef void *(*pipeline_input_t)(void *);
typedef int (*pipeline_output_t)(void *, void *);
typedef void (*pipeline_stage_t)(void *);
typedef void (*rtos_osal_entry_function_t)(void *);
typedef pthread_t rtos_osal_thread_t;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    void *items[2];
    unsigned count, capacity;
} rtos_osal_queue_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned live_frames, peak_frames, input_calls, output_calls, allow_output;
static pthread_t threads[2];
static unsigned thread_count;
static void *controls[8];
static unsigned control_count;
static rtos_osal_queue_t *queues[2];
static unsigned queue_count;

static void *pvPortMalloc(size_t size)
{
    assert(size == 2 * appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));
    void *result = malloc(size);
    assert(result != NULL);
    pthread_mutex_lock(&lock);
    ++live_frames;
    if (live_frames > peak_frames) peak_frames = live_frames;
    pthread_mutex_unlock(&lock);
    return result;
}

static void *rtos_osal_malloc(size_t size)
{
    assert(control_count < 8);
    void *result = malloc(size);
    assert(result != NULL);
    controls[control_count++] = result;
    return result;
}

static void rtos_osal_free(void *frame)
{
    free(frame);
    pthread_mutex_lock(&lock);
    assert(live_frames > 0);
    --live_frames;
    pthread_mutex_unlock(&lock);
}

static int rtos_osal_queue_create(rtos_osal_queue_t *queue, const char *name,
                                  unsigned capacity, unsigned item_size)
{
    (void)name;
    assert(capacity == 2 && item_size == sizeof(void *) && queue_count < 2);
    *queue = (rtos_osal_queue_t){.capacity = capacity};
    assert(pthread_mutex_init(&queue->lock, NULL) == 0);
    assert(pthread_cond_init(&queue->changed, NULL) == 0);
    queues[queue_count++] = queue;
    return 0;
}

static int rtos_osal_queue_send(rtos_osal_queue_t *queue, const void *item, unsigned timeout)
{
    assert(timeout == RTOS_OSAL_WAIT_FOREVER);
    pthread_mutex_lock(&queue->lock);
    while (queue->count == queue->capacity) pthread_cond_wait(&queue->changed, &queue->lock);
    memcpy(&queue->items[queue->count++], item, sizeof(void *));
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->lock);
    return 0;
}

static int rtos_osal_queue_receive(rtos_osal_queue_t *queue, void *item, unsigned timeout)
{
    assert(timeout == RTOS_OSAL_WAIT_FOREVER);
    pthread_mutex_lock(&queue->lock);
    while (queue->count == 0) pthread_cond_wait(&queue->changed, &queue->lock);
    memcpy(item, &queue->items[0], sizeof(void *));
    memmove(queue->items, queue->items + 1, --queue->count * sizeof(void *));
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->lock);
    return 0;
}

typedef struct {rtos_osal_entry_function_t entry; void *arg;} thread_start_t;
static void *thread_start(void *arg)
{
    thread_start_t start = *(thread_start_t *)arg;
    free(arg);
    start.entry(start.arg);
    return NULL;
}

static int rtos_osal_thread_create(rtos_osal_thread_t *thread, const char *name,
    rtos_osal_entry_function_t entry, void *arg, size_t stack, unsigned priority)
{
    (void)name;
    assert(thread == NULL && thread_count < 2 && stack > configMINIMAL_STACK_SIZE);
    assert(priority == appconfAUDIO_PIPELINE_TASK_PRIORITY);
    thread_start_t *start = malloc(sizeof(*start));
    assert(start != NULL);
    *start = (thread_start_t){entry, arg};
    assert(pthread_create(&threads[thread_count++], NULL, thread_start, start) == 0);
    return 0;
}

static int rtos_osal_task_yield(void)
{
    pthread_mutex_lock(&lock);
    unsigned done = output_calls >= 64;
    pthread_mutex_unlock(&lock);
    if (done) pthread_exit(NULL);
    return 0;
}

static void speaker_pipeline_input(void *ctx, int32_t **samples, size_t channels, size_t frames)
{
    (void)ctx;
    assert(channels == 2 && frames == appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE);
    pthread_mutex_lock(&lock);
    unsigned sequence = ++input_calls;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    int32_t *data = (int32_t *)samples;
    for (size_t i = 0; i < frames; ++i) {
        data[i] = (int32_t)(sequence * 1000 + i);
        data[frames + i] = -data[i];
    }
}

static int speaker_pipeline_output(void *ctx, int32_t **samples, size_t channels, size_t frames)
{
    (void)ctx;
    assert(channels == 2 && frames == appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE);
    pthread_mutex_lock(&lock);
    pthread_cond_broadcast(&changed);
    while (!allow_output) pthread_cond_wait(&changed, &lock);
    unsigned sequence = ++output_calls;
    pthread_mutex_unlock(&lock);
    int32_t *data = (int32_t *)samples;
    for (size_t i = 0; i < frames; ++i) {
        assert(data[i] == (int32_t)(sequence * 1000 + i));
        assert(data[frames + i] == -data[i]);
    }
    return 1;
}

/* The generic API casts opaque frame pointers; retain other UB/ASan checks. */
#include "sat1_speaker_pipeline.inc"

int main(void)
{
    speaker_pipeline_init(NULL, NULL);
    pthread_mutex_lock(&lock);
    while (input_calls == 0) pthread_cond_wait(&changed, &lock);
    struct timespec until;
    assert(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_nsec += 100000000;
    if (until.tv_nsec >= 1000000000) {until.tv_sec++; until.tv_nsec -= 1000000000;}
    while (input_calls == 1) {
        int result = pthread_cond_timedwait(&changed, &lock, &until);
        if (result == ETIMEDOUT) break;
        assert(result == 0);
    }
    /* A blocked reference/DAC consumer must stop the next input allocation. */
    assert(input_calls == 1 && live_frames == 1 && peak_frames == 1);
    allow_output = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < thread_count; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(input_calls == 64 && output_calls == 64 && live_frames == 0 && peak_frames == 1);
    for (unsigned i = 0; i < queue_count; ++i) {
        pthread_cond_destroy(&queues[i]->changed);
        pthread_mutex_destroy(&queues[i]->lock);
    }
    for (unsigned i = 0; i < control_count; ++i) free(controls[i]);
    pthread_cond_destroy(&changed);
    pthread_mutex_destroy(&lock);
    return 0;
}
