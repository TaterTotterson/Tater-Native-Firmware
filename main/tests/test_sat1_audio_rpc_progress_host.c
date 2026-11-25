/* Run the actual DSP output and intertile TX with a blocked frame receiver. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define RTOS_OSAL_PORT_WAIT_FOREVER UINT32_MAX
#define AUDIO_PIPELINE_FREE_FRAME 1
#define xassert assert
typedef struct {pthread_mutex_t lock; unsigned c;} rtos_intertile_t;
typedef struct {int32_t samples[16];} frame_data_t;
static rtos_intertile_t control_link = {PTHREAD_MUTEX_INITIALIZER, 0};
static rtos_intertile_t audio_link = {PTHREAD_MUTEX_INITIALIZER, 1};
static rtos_intertile_t *intertile_ctx = &control_link;
static __attribute__((unused)) rtos_intertile_t *intertile_audio_ctx = &audio_link;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local unsigned selected_port;
static unsigned dsp_waiting, reply_received, frame_released, dsp_completed;
static frame_data_t frame;

static void rtos_osal_mutex_get(pthread_mutex_t *lock, unsigned wait)
{
    assert(wait == RTOS_OSAL_PORT_WAIT_FOREVER);
    assert(pthread_mutex_lock(lock) == 0);
}
static void rtos_osal_mutex_put(pthread_mutex_t *lock)
{
    assert(pthread_mutex_unlock(lock) == 0);
}
static void s_chan_out_byte(unsigned channel, uint8_t port)
{
    assert(channel <= 1);
    selected_port = port;
}
static void s_chan_out_word(unsigned channel, size_t length)
{
    assert(channel <= 1 && length > 0);
}
static void s_chan_out_buf_byte(unsigned channel, const void *data, size_t length)
{
    assert(channel <= 1 && data != NULL && length > 0);
    assert(pthread_mutex_lock(&state_lock) == 0);
    if (selected_port == 7) {
        /* Receiver has no frame slot until I2S output receives its RPC reply. */
        assert(data == &frame && length == sizeof(frame));
        dsp_waiting = 1;
        pthread_cond_broadcast(&changed);
        while (!frame_released) pthread_cond_wait(&changed, &state_lock);
    } else {
        assert(selected_port == 14);
        reply_received = 1;
        pthread_cond_broadcast(&changed);
    }
    assert(pthread_mutex_unlock(&state_lock) == 0);
}

#include "sat1_audio_rpc_callbacks.inc"

static void *dsp_sender(void *arg)
{
    (void)arg;
    assert(audio_pipeline_output_i(&frame, NULL) == AUDIO_PIPELINE_FREE_FRAME);
    pthread_mutex_lock(&state_lock);
    dsp_completed = 1;
    pthread_mutex_unlock(&state_lock);
    return NULL;
}
static void *i2s_reply(void *arg)
{
    (void)arg;
    unsigned response = 1;
    rtos_intertile_tx(intertile_ctx, 14, &response, sizeof(response));
    return NULL;
}

int main(void)
{
    pthread_t audio, reply;
    assert(pthread_create(&audio, NULL, dsp_sender, NULL) == 0);
    pthread_mutex_lock(&state_lock);
    while (!dsp_waiting) pthread_cond_wait(&changed, &state_lock);
    pthread_mutex_unlock(&state_lock);
    assert(pthread_create(&reply, NULL, i2s_reply, NULL) == 0);
    pthread_mutex_lock(&state_lock);
    struct timespec until;
    assert(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_sec++;
    while (!reply_received) {
        int result = pthread_cond_timedwait(&changed, &state_lock, &until);
        if (result == ETIMEDOUT) break;
        assert(result == 0);
    }
    assert(reply_received && "I2S reply must progress while DSP send is blocked");
    assert(!dsp_completed);
    frame_released = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&state_lock);
    assert(pthread_join(audio, NULL) == 0 && pthread_join(reply, NULL) == 0);
    assert(dsp_completed);
    pthread_cond_destroy(&changed);
    pthread_mutex_destroy(&state_lock);
    pthread_mutex_destroy(&control_link.lock);
    pthread_mutex_destroy(&audio_link.lock);
    return 0;
}
