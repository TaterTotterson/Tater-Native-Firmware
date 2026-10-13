#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_i2s.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_timer.h"

typedef struct {
    uint32_t submitted_frames;
    uint32_t completed_frames;
    int64_t completed_at_us;
    uint32_t completion_sequence;
    uint8_t bytes_per_frame;
} tater_audio_render_clock_state_t;

/*
 * Wake the Sendspin render-feedback task from the I2S completion callback.
 * The implementation is a no-op until Sendspin has created that task.
 */
bool IRAM_ATTR tater_sendspin_notify_render_from_isr(void);

static inline void tater_audio_render_clock_reset(
    tater_audio_render_clock_state_t *state
)
{
    if (!state) {
        return;
    }
    __atomic_fetch_add(&state->completion_sequence, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&state->submitted_frames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&state->completed_frames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&state->completed_at_us, 0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&state->completion_sequence, 1, __ATOMIC_RELEASE);
}

static IRAM_ATTR bool tater_audio_render_clock_on_sent(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_ctx
)
{
    (void)handle;
    tater_audio_render_clock_state_t *state = user_ctx;
    if (!state || !event || state->bytes_per_frame == 0) {
        return false;
    }
    uint32_t event_frames = (uint32_t)(event->size / state->bytes_per_frame);
    uint32_t submitted = __atomic_load_n(&state->submitted_frames, __ATOMIC_RELAXED);
    uint32_t completed = __atomic_load_n(&state->completed_frames, __ATOMIC_RELAXED);
    uint32_t pending = submitted - completed;
    if (event_frames > pending) {
        event_frames = pending;
    }
    if (event_frames > 0) {
        // Capture the DMA completion instant in the ISR.  Using the later polling-task time can
        // move the reported playhead by several milliseconds when that task is briefly delayed,
        // which makes Sendspin perform an audible correction even though I2S stayed healthy.
        __atomic_fetch_add(&state->completion_sequence, 1, __ATOMIC_ACQ_REL);
        state->completed_at_us = esp_timer_get_time();
        __atomic_fetch_add(&state->completed_frames, event_frames, __ATOMIC_RELAXED);
        __atomic_fetch_add(&state->completion_sequence, 1, __ATOMIC_RELEASE);
        return tater_sendspin_notify_render_from_isr();
    }
    return false;
}

static inline esp_err_t tater_audio_render_clock_register(
    tater_audio_render_clock_state_t *state,
    i2s_chan_handle_t channel,
    uint8_t bytes_per_frame
)
{
    if (!state || !channel || bytes_per_frame == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    state->bytes_per_frame = bytes_per_frame;
    tater_audio_render_clock_reset(state);
    const i2s_event_callbacks_t callbacks = {
        .on_sent = tater_audio_render_clock_on_sent,
    };
    return i2s_channel_register_event_callback(channel, &callbacks, state);
}

static inline esp_err_t tater_audio_render_clock_write(
    tater_audio_render_clock_state_t *state,
    i2s_chan_handle_t channel,
    const void *source,
    size_t size,
    size_t *bytes_written,
    uint32_t timeout_ms
)
{
    uint32_t reserved_frames = 0;
    if (state && state->bytes_per_frame > 0 && bytes_written) {
        reserved_frames = (uint32_t)(size / state->bytes_per_frame);
        if (reserved_frames > 0) {
            __atomic_fetch_add(
                &state->submitted_frames,
                reserved_frames,
                __ATOMIC_RELAXED
            );
        }
    }
    esp_err_t err = i2s_channel_write(channel, source, size, bytes_written, timeout_ms);
    if (reserved_frames > 0) {
        uint32_t written_frames = (uint32_t)(*bytes_written / state->bytes_per_frame);
        if (written_frames < reserved_frames) {
            __atomic_fetch_sub(
                &state->submitted_frames,
                reserved_frames - written_frames,
                __ATOMIC_RELAXED
            );
        }
    }
    return err;
}

static inline esp_err_t tater_audio_render_clock_preload(
    tater_audio_render_clock_state_t *state,
    i2s_chan_handle_t channel,
    const void *source,
    size_t size,
    size_t *bytes_loaded
)
{
    uint32_t reserved_frames = 0;
    if (state && state->bytes_per_frame > 0 && bytes_loaded) {
        reserved_frames = (uint32_t)(size / state->bytes_per_frame);
        if (reserved_frames > 0) {
            __atomic_fetch_add(
                &state->submitted_frames,
                reserved_frames,
                __ATOMIC_RELAXED
            );
        }
    }
    esp_err_t err = i2s_channel_preload_data(channel, source, size, bytes_loaded);
    if (reserved_frames > 0) {
        uint32_t loaded_frames = (uint32_t)(*bytes_loaded / state->bytes_per_frame);
        if (loaded_frames < reserved_frames) {
            __atomic_fetch_sub(
                &state->submitted_frames,
                reserved_frames - loaded_frames,
                __ATOMIC_RELAXED
            );
        }
    }
    return err;
}

static inline bool tater_audio_render_clock_snapshot(
    const tater_audio_render_clock_state_t *state,
    tater_audio_render_clock_t *out
)
{
    if (!state || !out || state->bytes_per_frame == 0) {
        return false;
    }
    out->submitted_frames = __atomic_load_n(&state->submitted_frames, __ATOMIC_RELAXED);
    uint32_t sequence_before = 0;
    uint32_t sequence_after = 0;
    do {
        sequence_before = __atomic_load_n(&state->completion_sequence, __ATOMIC_ACQUIRE);
        if ((sequence_before & 1U) != 0U) {
            continue;
        }
        out->completed_frames = __atomic_load_n(&state->completed_frames, __ATOMIC_RELAXED);
        out->completed_at_us = state->completed_at_us;
        sequence_after = __atomic_load_n(&state->completion_sequence, __ATOMIC_ACQUIRE);
    } while (sequence_before != sequence_after || (sequence_after & 1U) != 0U);
    return true;
}
