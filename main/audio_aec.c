#include "audio_aec.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio_i2s.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "native_settings.h"

static const char *TAG = "tater_aec";

#define AEC_REF_RING_SAMPLES 8192
#define AEC_FILTER_TAPS 96
#define AEC_DEFAULT_DELAY_MS 85
#define AEC_ACTIVE_TAIL_US 220000
#define AEC_MIN_REF_LEVEL 0.0015f
#define AEC_MIN_SPEAKER_LEVEL 0.012f
#define AEC_EPSILON 0.00004f

#ifndef TATER_ENABLE_EXPERIMENTAL_AEC
#define TATER_ENABLE_EXPERIMENTAL_AEC 0
#endif

static int16_t s_reference_ring[AEC_REF_RING_SAMPLES];
static size_t s_reference_write;
static size_t s_reference_count;
static uint32_t s_reference_resample_accum;
static int64_t s_last_reference_us;
static portMUX_TYPE s_reference_lock = portMUX_INITIALIZER_UNLOCKED;

static float s_filter[AEC_FILTER_TAPS];
static float s_history[AEC_FILTER_TAPS];
static size_t s_history_pos;

static tater_audio_aec_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static float clamp01(float value)
{
    if (value <= 0.0f) {
        return 0.0f;
    }
    if (value >= 1.0f) {
        return 1.0f;
    }
    return value;
}

static int16_t clamp_s16_from_float(float value)
{
    if (value > 32767.0f) {
        return INT16_MAX;
    }
    if (value < -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)(value + (value >= 0.0f ? 0.5f : -0.5f));
}

static float normalized_abs_mean(const int16_t *samples, size_t count)
{
    if (!samples || count == 0) {
        return 0.0f;
    }

    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t sample = samples[i];
        sum += sample < 0 ? (uint32_t)(-sample) : (uint32_t)sample;
    }
    return ((float)sum / (float)count) / 32768.0f;
}

static size_t configured_delay_samples(uint8_t delay_ms)
{
    size_t samples = ((size_t)delay_ms * (size_t)TATER_MIC_SAMPLE_RATE) / 1000U;
    if (samples + TATER_MIC_CHUNK_FRAMES + AEC_FILTER_TAPS >= AEC_REF_RING_SAMPLES) {
        samples = AEC_REF_RING_SAMPLES - TATER_MIC_CHUNK_FRAMES - AEC_FILTER_TAPS - 1;
    }
    return samples;
}

static void reference_push(int16_t sample)
{
    s_reference_ring[s_reference_write] = sample;
    s_reference_write = (s_reference_write + 1) % AEC_REF_RING_SAMPLES;
    if (s_reference_count < AEC_REF_RING_SAMPLES) {
        s_reference_count++;
    }
}

static void copy_delayed_reference(int16_t *out, size_t count, size_t delay_samples)
{
    if (!out || count == 0) {
        return;
    }

    memset(out, 0, count * sizeof(out[0]));
    portENTER_CRITICAL(&s_reference_lock);
    size_t available = s_reference_count;
    if (available >= delay_samples + count) {
        size_t start = (s_reference_write + AEC_REF_RING_SAMPLES - delay_samples - count) % AEC_REF_RING_SAMPLES;
        for (size_t i = 0; i < count; i++) {
            out[i] = s_reference_ring[(start + i) % AEC_REF_RING_SAMPLES];
        }
    }
    portEXIT_CRITICAL(&s_reference_lock);
}

static bool reference_recent(int64_t now_us)
{
    int64_t last = 0;
    portENTER_CRITICAL(&s_reference_lock);
    last = s_last_reference_us;
    portEXIT_CRITICAL(&s_reference_lock);
    return last > 0 && now_us >= last && (now_us - last) <= AEC_ACTIVE_TAIL_US;
}

static void update_reference_stats(size_t reference_frames, float speaker_level, uint8_t strength_percent, uint8_t delay_ms)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.enabled = true;
    s_stats.reference_frames += (uint32_t)reference_frames;
    s_stats.last_speaker_level = speaker_level;
    s_stats.strength_percent = strength_percent;
    s_stats.delay_ms = delay_ms;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void update_stats(
    bool enabled,
    bool active,
    size_t processed_frames,
    size_t reference_frames,
    float mic_level,
    float reference_level,
    float speaker_level,
    float output_gain,
    uint8_t strength_percent,
    uint8_t delay_ms
)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.enabled = enabled;
    s_stats.active = active;
    s_stats.processed_frames += (uint32_t)processed_frames;
    if (active) {
        s_stats.active_frames += (uint32_t)processed_frames;
    }
    s_stats.reference_frames += (uint32_t)reference_frames;
    s_stats.last_mic_level = mic_level;
    s_stats.last_reference_level = reference_level;
    s_stats.last_speaker_level = speaker_level;
    s_stats.last_output_gain = output_gain;
    s_stats.strength_percent = strength_percent;
    s_stats.delay_ms = delay_ms;
    portEXIT_CRITICAL(&s_stats_lock);
}

void tater_audio_aec_init(void)
{
    memset(s_reference_ring, 0, sizeof(s_reference_ring));
    memset(s_filter, 0, sizeof(s_filter));
    memset(s_history, 0, sizeof(s_history));
    s_reference_write = 0;
    s_reference_count = 0;
    s_reference_resample_accum = 0;
    s_history_pos = 0;
    s_last_reference_us = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.enabled = false;
    s_stats.strength_percent = 70;
    s_stats.delay_ms = AEC_DEFAULT_DELAY_MS;
    ESP_LOGI(TAG, "aec initialized taps=%u ring=%u sample_rate=%u", AEC_FILTER_TAPS, AEC_REF_RING_SAMPLES, TATER_MIC_SAMPLE_RATE);
}

void tater_audio_aec_note_speaker_frames(const int16_t *stereo_frames, size_t frame_count)
{
#if !TATER_ENABLE_EXPERIMENTAL_AEC
    (void)stereo_frames;
    (void)frame_count;
    return;
#else
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (!settings || !settings->aec_enabled || !stereo_frames || frame_count == 0) {
        return;
    }

    size_t reference_frames = 0;
    portENTER_CRITICAL(&s_reference_lock);
    for (size_t frame = 0; frame < frame_count; frame++) {
        int32_t left = stereo_frames[frame * TATER_SPK_CHANNELS];
        int32_t right = TATER_SPK_CHANNELS > 1 ? stereo_frames[(frame * TATER_SPK_CHANNELS) + 1] : left;
        int16_t mono = (int16_t)((left + right) / 2);

        s_reference_resample_accum += TATER_MIC_SAMPLE_RATE;
        while (s_reference_resample_accum >= TATER_SPK_SAMPLE_RATE) {
            reference_push(mono);
            reference_frames++;
            s_reference_resample_accum -= TATER_SPK_SAMPLE_RATE;
        }
    }
    s_last_reference_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_reference_lock);

    if (reference_frames > 0) {
        update_reference_stats(reference_frames, tater_audio_speaker_level(), settings->aec_strength_percent, settings->aec_delay_ms);
    }
#endif
}

void tater_audio_aec_process_mic(int16_t *mono_frames, size_t frame_count)
{
#if !TATER_ENABLE_EXPERIMENTAL_AEC
    update_stats(false, false, frame_count, 0, normalized_abs_mean(mono_frames, frame_count), 0.0f, tater_audio_speaker_level(), 1.0f, 0, AEC_DEFAULT_DELAY_MS);
    return;
#else
    const tater_live_settings_t *settings = tater_live_settings_get();
    bool enabled = settings && settings->aec_enabled;
    uint8_t strength_percent = settings ? settings->aec_strength_percent : 70;
    uint8_t delay_ms = settings ? settings->aec_delay_ms : AEC_DEFAULT_DELAY_MS;
    if (!enabled || !mono_frames || frame_count == 0 || strength_percent == 0) {
        update_stats(enabled, false, frame_count, 0, normalized_abs_mean(mono_frames, frame_count), 0.0f, tater_audio_speaker_level(), 1.0f, strength_percent, delay_ms);
        return;
    }

    while (frame_count > 0) {
        size_t chunk = frame_count > TATER_MIC_CHUNK_FRAMES ? TATER_MIC_CHUNK_FRAMES : frame_count;
        int16_t reference[TATER_MIC_CHUNK_FRAMES];
        copy_delayed_reference(reference, chunk, configured_delay_samples(delay_ms));

        float mic_level = normalized_abs_mean(mono_frames, chunk);
        float reference_level = normalized_abs_mean(reference, chunk);
        float speaker_level = tater_audio_speaker_level();
        int64_t now_us = esp_timer_get_time();
        bool active = speaker_level >= AEC_MIN_SPEAKER_LEVEL || reference_level >= AEC_MIN_REF_LEVEL || reference_recent(now_us);
        float strength = clamp01((float)strength_percent / 100.0f);
        bool near_end = active
            && mic_level > 0.018f
            && mic_level > (reference_level * 2.4f + 0.010f)
            && mic_level > (speaker_level * 1.7f + 0.025f);
        bool adapt = active && !near_end && reference_level >= AEC_MIN_REF_LEVEL;
        float playback_factor = clamp01((speaker_level * 5.5f) + (reference_level * 8.0f));
        float residual_gain = 1.0f;
        if (active) {
            if (near_end && settings->barge_in_enabled) {
                residual_gain = 1.0f - (strength * playback_factor * 0.12f);
            } else if (near_end) {
                residual_gain = 1.0f - (strength * playback_factor * 0.22f);
            } else {
                residual_gain = 1.0f - (strength * playback_factor * 0.68f);
            }
            if (residual_gain < 0.28f) {
                residual_gain = 0.28f;
            }
        }

        for (size_t i = 0; i < chunk; i++) {
            float x = (float)reference[i] / 32768.0f;
            float y = (float)mono_frames[i] / 32768.0f;

            s_history[s_history_pos] = x;
            float y_hat = 0.0f;
            float energy = AEC_EPSILON;
            size_t hist = s_history_pos;
            for (size_t tap = 0; tap < AEC_FILTER_TAPS; tap++) {
                float value = s_history[hist];
                y_hat += s_filter[tap] * value;
                energy += value * value;
                hist = hist == 0 ? (AEC_FILTER_TAPS - 1) : (hist - 1);
            }

            float error = y - y_hat;
            if (adapt) {
                float mu = (0.028f + (0.040f * strength)) / energy;
                hist = s_history_pos;
                for (size_t tap = 0; tap < AEC_FILTER_TAPS; tap++) {
                    float updated = s_filter[tap] + (mu * error * s_history[hist]);
                    if (updated > 1.6f) {
                        updated = 1.6f;
                    } else if (updated < -1.6f) {
                        updated = -1.6f;
                    }
                    s_filter[tap] = updated;
                    hist = hist == 0 ? (AEC_FILTER_TAPS - 1) : (hist - 1);
                }
            }

            float out = error * residual_gain;
            mono_frames[i] = clamp_s16_from_float(out * 32768.0f);
            s_history_pos = (s_history_pos + 1) % AEC_FILTER_TAPS;
        }

        update_stats(enabled, active, chunk, 0, mic_level, reference_level, speaker_level, residual_gain, strength_percent, delay_ms);
        mono_frames += chunk;
        frame_count -= chunk;
    }
#endif
}

bool tater_audio_aec_stats_snapshot(tater_audio_aec_stats_t *out)
{
    if (!out) {
        return false;
    }
    portENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
    return true;
}
