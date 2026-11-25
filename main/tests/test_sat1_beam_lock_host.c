/* Compile with callbacks extracted from main.c; exercise their actual locking. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "beamforming/sat1_beamformer.h"

#define MIC_ARRAY_CONFIG_MIC_COUNT 4
#define MIC_ARRAY_CONFIG_SAMPLES_PER_FRAME 240
#define appconfAUDIO_PIPELINE_CHANNELS 4
#define appconfSAT1_AEC4 1
#define appconfSAT1_AUDIO_BENCH 0
#define portMAX_DELAY ((unsigned)-1)

static pthread_mutex_t beam_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t control_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t control_changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t *sat1_beam_mutex = &beam_mutex;
static sat1_beamformer_t sat1_beamformer;
static doa_estimator_state_t sat1_beam_direction;
static doa_estimator_state_t live_direction;
static void *mic_array_ctx;
static bool pause_direction, direction_started, allow_direction, post_finished;
static unsigned generation, expected_generation;
static int32_t capture_samples[4][240], post_samples[4][240];

static void xSemaphoreTake(pthread_mutex_t *mutex, unsigned timeout)
{
    (void)timeout;
    assert(pthread_mutex_lock(mutex) == 0);
}

static void xSemaphoreGive(pthread_mutex_t *mutex)
{
    assert(pthread_mutex_unlock(mutex) == 0);
}

static size_t rtos_mic_array_rx(void *ctx, int32_t **samples,
                                size_t frames, unsigned timeout)
{
    (void)ctx;
    (void)timeout;
    assert(frames == 240);
    memset(samples, 0, sizeof(capture_samples));
    return frames;
}

void sat1_beamformer_analyze_frame(sat1_beamformer_t *beamformer,
                                   const int32_t *microphones[4], size_t frames)
{
    assert(frames == 240 && microphones[3] == capture_samples[3]);
    beamformer->initialized = 1;
    beamformer->active_mic_mask = 15;
}

void doa_estimator_set_mic_health(uint8_t mask)
{
    assert(mask == 15);
}

void doa_estimator_process_frame_4(const int32_t *east, const int32_t *west,
                                   const int32_t *north, const int32_t *south,
                                   size_t frames)
{
    assert(east && west && north && south && frames == 240);
    assert(pthread_mutex_lock(&control_mutex) == 0);
    ++generation;
    /* Deliberately leave an incoherent live result while the estimator waits. */
    live_direction.frame_counter = generation;
    direction_started = true;
    assert(pthread_cond_broadcast(&control_changed) == 0);
    while (pause_direction && !allow_direction) {
        assert(pthread_cond_wait(&control_changed, &control_mutex) == 0);
    }
    live_direction.sample_delay_q8 = (int16_t)(generation * 100);
    live_direction.vertical_delay_q8 = (int16_t)(generation * 3);
    live_direction.flags = (uint8_t)generation;
    for (unsigned mic = 0; mic < 4; ++mic) {
        live_direction.mic_energy[mic] = generation + mic;
    }
    assert(pthread_mutex_unlock(&control_mutex) == 0);
}

void doa_estimator_get_state(doa_estimator_state_t *direction)
{
    *direction = live_direction;
}

void sat1_beamformer_process_calibrated_frame(sat1_beamformer_t *beamformer,
    int32_t *output, const int32_t *microphones[4], size_t frames,
    const doa_estimator_state_t *direction)
{
    assert(beamformer->initialized && microphones[3] == post_samples[3]);
    assert(frames == 240 && direction->frame_counter == expected_generation);
    assert(direction->sample_delay_q8 == (int16_t)(expected_generation * 100));
    assert(direction->vertical_delay_q8 == (int16_t)(expected_generation * 3));
    assert(direction->flags == expected_generation);
    for (unsigned mic = 0; mic < 4; ++mic) {
        assert(direction->mic_energy[mic] == expected_generation + mic);
    }
    for (unsigned i = 0; i < 240; ++i) output[i] = (int32_t)(expected_generation + i);
}

#include "sat1_beam_callbacks.inc"

static void *capture(void *arg)
{
    (void)arg;
    assert(receive_mic_array_frame((int32_t **)capture_samples, 240, 0) == 240);
    return NULL;
}

static void *post(void *arg)
{
    (void)arg;
    sat1_post_aec_beam(post_samples);
    assert(pthread_mutex_lock(&control_mutex) == 0);
    post_finished = true;
    assert(pthread_cond_broadcast(&control_changed) == 0);
    assert(pthread_mutex_unlock(&control_mutex) == 0);
    return NULL;
}

int main(void)
{
    memset(&sat1_beam_direction, 0, sizeof(sat1_beam_direction));
    capture(NULL);
    expected_generation = 1;
    sat1_post_aec_beam(post_samples);
    assert(post_samples[0][239] == 240);

    pthread_t capture_thread, post_thread;
    pause_direction = true;
    direction_started = false;
    assert(pthread_create(&capture_thread, NULL, capture, NULL) == 0);
    assert(pthread_mutex_lock(&control_mutex) == 0);
    while (!direction_started) {
        assert(pthread_cond_wait(&control_changed, &control_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&control_mutex) == 0);
    assert(pthread_create(&post_thread, NULL, post, NULL) == 0);

    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec++;
    assert(pthread_mutex_lock(&control_mutex) == 0);
    while (!post_finished) {
        int result = pthread_cond_timedwait(&control_changed, &control_mutex, &deadline);
        if (result == ETIMEDOUT) break;
        assert(result == 0);
    }
    bool finished_during_analysis = post_finished;
    allow_direction = true;
    assert(pthread_cond_broadcast(&control_changed) == 0);
    assert(pthread_mutex_unlock(&control_mutex) == 0);
    assert(pthread_join(capture_thread, NULL) == 0);
    assert(pthread_join(post_thread, NULL) == 0);
    assert(finished_during_analysis);

    expected_generation = 2;
    sat1_post_aec_beam(post_samples);
    assert(post_samples[0][239] == 241);
    return 0;
}
