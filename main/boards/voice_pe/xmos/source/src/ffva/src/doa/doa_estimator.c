#include "doa_estimator.h"

#include <limits.h>
#include <string.h>
#include "voicepe_audio_protocol.h"

#ifndef VOICEPE_DOA_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#define STATE_LOCK() taskENTER_CRITICAL()
#define STATE_UNLOCK() taskEXIT_CRITICAL()
#else
#define STATE_LOCK() ((void)0)
#define STATE_UNLOCK() ((void)0)
#endif

#define DOA_ESTIMATOR_SAMPLE_SHIFT (12)
#define DOA_ESTIMATOR_MIN_ENERGY   (512)

static doa_estimator_state_t latest_state;
static uint8_t host_hints;
static uint32_t host_hints_ms;
static bool host_hints_received;
/* Only the IC/VNR stage owns speech_hold. */
static unsigned speech_hold;

uint16_t doa_estimator_instant_vnr(uint16_t before_q15, uint16_t after_q15,
                                  uint32_t alpha_q30)
{
    const uint32_t one = 1u << 30;
    if (before_q15 > 32768) before_q15 = 32768;
    if (after_q15 > 32768) after_q15 = 32768;
    if (alpha_q30 >= one) return after_q15;
    int64_t numerator = (int64_t)after_q15 * one - (int64_t)before_q15 * alpha_q30;
    uint32_t denominator = one - alpha_q30;
    if (numerator <= 0) return 0;
    if (numerator >= (int64_t)denominator * 32768) return 32768;
#if defined(__XS3A__)
    uint32_t quotient, remainder;
    numerator += denominator / 2;
    __asm__("ldivu %0, %1, %2, %3, %4"
        : "=&r"(quotient), "=&r"(remainder)
        : "r"((uint32_t)((uint64_t)numerator >> 32)), "r"((uint32_t)numerator), "r"(denominator));
    return quotient > 32768 ? 32768 : (uint16_t)quotient;
#else
    int64_t value = (numerator + denominator / 2) / denominator;
    return value >= 32768 ? 32768 : (uint16_t)value;
#endif
}

static int32_t scaled_sample(int32_t sample)
{
    return sample >> DOA_ESTIMATOR_SAMPLE_SHIFT;
}

static uint16_t lag_correlation(const int32_t *mic0,
                               const int32_t *mic1,
                               size_t frame_count,
                               int lag)
{
    int64_t corr = 0, sum0 = 0, sum1 = 0;
    uint64_t energy0 = 0, energy1 = 0;
    size_t start0 = 0;
    size_t start1 = 0;
    size_t count = frame_count;

    if (lag > 0) {
        start1 = (size_t)lag;
        count -= (size_t)lag;
    } else if (lag < 0) {
        start0 = (size_t)-lag;
        count -= (size_t)-lag;
    }

    for (size_t i = 0; i < count; i++) {
        int64_t a = scaled_sample(mic0[start0 + i]);
        int64_t b = scaled_sample(mic1[start1 + i]);
        corr += a * b;
        sum0 += a; sum1 += b;
        energy0 += (uint64_t)(a * a); energy1 += (uint64_t)(b * b);
    }
    /* Mean-centred normalized correlation is insensitive to microphone gain
     * and DC. Neighboring lags belong to the same broad speech peak, so their
     * tiny difference must not quantize otherwise good confidence to zero. */
    corr = (int64_t)count * corr - sum0 * sum1;
    energy0 = count * energy0 - (uint64_t)(sum0 * sum0);
    energy1 = count * energy1 - (uint64_t)(sum1 * sum1);
    uint64_t magnitude = corr < 0 ? (uint64_t)-corr : (uint64_t)corr;
    while (energy0 > 65535 || energy1 > 65535) {
        energy0 >>= 1; energy1 >>= 1; magnitude >>= 1;
    }
    uint32_t denominator = (uint32_t)(energy0 * energy1);
    if (!denominator) return 0;
    uint64_t numerator = magnitude * magnitude * 32768u;
    if (numerator >= (uint64_t)denominator * 32768u) return 32768;
#if defined(__XS3A__)
    uint32_t quotient, remainder;
    __asm__("ldivu %0, %1, %2, %3, %4"
        : "=&r"(quotient), "=&r"(remainder)
        : "r"((uint32_t)(numerator >> 32)), "r"((uint32_t)numerator), "r"(denominator));
    return (uint16_t)quotient;
#else
    return (uint16_t)(numerator / denominator);
#endif
}

void doa_estimator_analyze_frame(const int32_t *mic0,
                                 const int32_t *mic1,
                                 size_t frame_count,
                                 doa_estimator_state_t *state)
{
    doa_estimator_state_t next = {0};
    int64_t energy = 0;
    int best_corr = -1;
    int best_lag = 0;

    if (state == NULL) return;
    *state = next;
    if (mic0 == NULL || mic1 == NULL || frame_count <= (2 * DOA_ESTIMATOR_MAX_LAG_SAMPLES)) {
        return;
    }

    for (size_t i = 0; i < frame_count; i++) {
        int32_t s0 = scaled_sample(mic0[i]);
        int32_t s1 = scaled_sample(mic1[i]);
        energy += (int64_t)s0 * (int64_t)s0;
        energy += (int64_t)s1 * (int64_t)s1;
    }

    for (int lag = -DOA_ESTIMATOR_MAX_LAG_SAMPLES;
         lag <= DOA_ESTIMATOR_MAX_LAG_SAMPLES;
         lag++) {
        int corr = lag_correlation(mic0, mic1, frame_count, lag);

        if (corr > best_corr) {
            best_corr = corr;
            best_lag = lag;
        }
    }

    next.energy = energy > UINT32_MAX ? UINT32_MAX : (uint32_t)energy;

    if (energy >= DOA_ESTIMATOR_MIN_ENERGY && best_corr >= 8192) {
        next.sample_delay = (int16_t)best_lag;
        next.confidence = (uint8_t)((uint32_t)best_corr * 255 / 32768);
        next.flags = DOA_ESTIMATOR_FLAG_VALID;
    }

    *state = next;
}

static void store_state(doa_estimator_state_t *state)
{
    STATE_LOCK();
    state->frame_counter = latest_state.frame_counter + 1;
    latest_state = *state;
    STATE_UNLOCK();
}

void doa_estimator_process_frame(const int32_t *mic0, const int32_t *mic1,
                                 size_t frame_count)
{
    /* Retain the ungated direction API for the other reference pipelines. */
    doa_estimator_state_t state;
    doa_estimator_analyze_frame(mic0, mic1, frame_count, &state);
    store_state(&state);
}

void doa_estimator_set_host_hints(uint8_t flags, uint32_t now_ms)
{
    STATE_LOCK();
    host_hints = flags & VOICEPE_DOA_HINT_MASK;
    host_hints_ms = now_ms;
    host_hints_received = true;
    STATE_UNLOCK();
}

void doa_estimator_publish_frame(const doa_estimator_state_t *candidate,
                                 bool speech_present, bool reference_active,
                                 bool echo_dominated, uint32_t now_ms)
{
    uint8_t hints;
    bool fresh;
    STATE_LOCK();
    hints = host_hints;
    fresh = host_hints_received &&
        (uint32_t)(now_ms - host_hints_ms) < VOICEPE_DOA_HINT_TTL_MS;
    STATE_UNLOCK();
    if (!fresh) hints = 0;

    bool playback = reference_active || (hints & VOICEPE_DOA_HINT_PLAYBACK);
    bool listening = (hints & VOICEPE_DOA_HINT_LISTENING) != 0;
    /* A listening hint relaxes the conservative echo-correlation veto, but
     * still requires speech evidence. Playback alone never disables tracking.
     * Without a host the local reference and VNR evidence remain sufficient. */
    bool speech = speech_present && (!playback || !echo_dominated || listening);
    if (speech) speech_hold = 6; /* 90 ms, no queued audio or endpoint change. */
    else if (speech_hold) --speech_hold;

    doa_estimator_state_t state = *candidate;
    state.flags = 0;
    if ((candidate->flags & DOA_ESTIMATOR_FLAG_VALID) && speech_hold &&
        (!playback || !echo_dominated || listening)) {
        state.flags |= DOA_ESTIMATOR_FLAG_VALID;
    } else {
        state.confidence = 0;
    }
    if (speech_hold) state.flags |= VOICEPE_DOA_FLAG_SPEECH;
    if (playback) state.flags |= VOICEPE_DOA_FLAG_PLAYBACK;
    if (listening) state.flags |= VOICEPE_DOA_FLAG_LISTENING;
    if (fresh) state.flags |= VOICEPE_DOA_FLAG_HINTS_FRESH;
    store_state(&state);
}

void doa_estimator_reset(void)
{
    STATE_LOCK();
    memset(&latest_state, 0, sizeof(latest_state));
    host_hints = 0;
    host_hints_ms = 0;
    host_hints_received = false;
    speech_hold = 0;
    STATE_UNLOCK();
}

void doa_estimator_get_state(doa_estimator_state_t *state)
{
    if (state == NULL) {
        return;
    }

    STATE_LOCK();
    *state = latest_state;
    STATE_UNLOCK();
}
