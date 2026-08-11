#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_i2s.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"

typedef struct {
    uint32_t submitted_frames;
    uint32_t completed_frames;
    uint8_t bytes_per_frame;
} tater_audio_render_clock_state_t;

static inline void tater_audio_render_clock_reset(
    tater_audio_render_clock_state_t *state
)
{
    if (!state) {
        return;
    }
    __atomic_store_n(&state->submitted_frames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&state->completed_frames, 0, __ATOMIC_RELAXED);
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
        __atomic_fetch_add(&state->completed_frames, event_frames, __ATOMIC_RELAXED);
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
    out->submitted_frames = __atomic_load_n(
        &state->submitted_frames,
        __ATOMIC_RELAXED
    );
    out->completed_frames = __atomic_load_n(
        &state->completed_frames,
        __ATOMIC_RELAXED
    );
    return true;
}
