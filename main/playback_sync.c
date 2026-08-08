#include "playback_sync.h"

#include <string.h>

#define PLAYBACK_SYNC_CHANNELS 2

static uint32_t playback_sync_abs_i32(int32_t value)
{
    return value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
}

void tater_playback_sync_slew_init(tater_playback_sync_slew_t *state)
{
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

void tater_playback_sync_slew_queue(
    tater_playback_sync_slew_t *state,
    int32_t correction_frames,
    uint32_t settle_output_frames,
    int32_t max_pending_frames
)
{
    if (!state || correction_frames == 0 || max_pending_frames <= 0) {
        return;
    }
    int64_t pending = (int64_t)state->pending_frames + correction_frames;
    if (pending > max_pending_frames) {
        pending = max_pending_frames;
    } else if (pending < -max_pending_frames) {
        pending = -max_pending_frames;
    }
    state->pending_frames = (int32_t)pending;
    uint32_t pending_abs = playback_sync_abs_i32(state->pending_frames);
    if (pending_abs == 0) {
        state->step_interval_output_frames = 0;
        state->frames_until_step = 0;
        return;
    }
    uint32_t settle_frames = settle_output_frames > 0 ? settle_output_frames : 1;
    uint32_t interval = settle_frames / pending_abs;
    state->step_interval_output_frames = interval > 0 ? interval : 1;
    if (
        state->frames_until_step == 0
        || state->frames_until_step > state->step_interval_output_frames
    ) {
        state->frames_until_step = state->step_interval_output_frames;
    }
}

int32_t tater_playback_sync_slew_next_step(
    tater_playback_sync_slew_t *state,
    uint32_t output_frames
)
{
    if (!state || state->pending_frames == 0 || output_frames == 0) {
        return 0;
    }
    if (state->frames_until_step > output_frames) {
        state->frames_until_step -= output_frames;
        return 0;
    }
    uint32_t overshoot = output_frames - state->frames_until_step;
    int32_t step = state->pending_frames > 0 ? 1 : -1;
    state->pending_frames -= step;
    if (state->pending_frames == 0) {
        state->frames_until_step = 0;
        state->step_interval_output_frames = 0;
    } else {
        uint32_t interval = state->step_interval_output_frames > 0
            ? state->step_interval_output_frames
            : 1;
        state->frames_until_step = interval > overshoot ? interval - overshoot : 1;
    }
    return step;
}

void tater_playback_sync_slew_restore_step(
    tater_playback_sync_slew_t *state,
    int32_t correction_step
)
{
    if (!state || correction_step == 0) {
        return;
    }
    state->pending_frames += correction_step;
    state->frames_until_step = state->step_interval_output_frames > 0
        ? state->step_interval_output_frames
        : 1;
}

void tater_playback_sync_resample_stereo(
    const int16_t *input,
    size_t input_frames,
    int16_t *output,
    size_t output_frames
)
{
    if (!input || !output || input_frames == 0 || output_frames == 0) {
        return;
    }
    if (input_frames == output_frames) {
        if (input != output) {
            memcpy(output, input, output_frames * PLAYBACK_SYNC_CHANNELS * sizeof(int16_t));
        }
        return;
    }
    if (input_frames == 1 || output_frames == 1) {
        for (size_t frame = 0; frame < output_frames; frame++) {
            output[frame * PLAYBACK_SYNC_CHANNELS] = input[0];
            output[(frame * PLAYBACK_SYNC_CHANNELS) + 1] = input[1];
        }
        return;
    }

    const uint64_t step_q32 =
        (((uint64_t)input_frames - 1ULL) << 32) / ((uint64_t)output_frames - 1ULL);
    uint64_t position_q32 = 0;
    for (size_t frame = 0; frame < output_frames; frame++) {
        if (frame == output_frames - 1) {
            position_q32 = ((uint64_t)input_frames - 1ULL) << 32;
        }
        size_t left = (size_t)(position_q32 >> 32);
        if (left >= input_frames - 1) {
            left = input_frames - 1;
        }
        size_t right = left < input_frames - 1 ? left + 1 : left;
        uint32_t fraction = (uint32_t)position_q32;
        for (size_t channel = 0; channel < PLAYBACK_SYNC_CHANNELS; channel++) {
            int32_t a = input[(left * PLAYBACK_SYNC_CHANNELS) + channel];
            int32_t b = input[(right * PLAYBACK_SYNC_CHANNELS) + channel];
            int64_t interpolated =
                ((int64_t)a * (1LL << 32))
                + ((int64_t)(b - a) * (int64_t)fraction);
            output[(frame * PLAYBACK_SYNC_CHANNELS) + channel] =
                (int16_t)(interpolated >> 32);
        }
        position_q32 += step_q32;
    }
}

void tater_playback_sync_fade_in(
    int16_t *stereo_frames,
    size_t frame_count,
    uint32_t *frames_remaining,
    uint32_t total_frames
)
{
    if (!stereo_frames || !frames_remaining || *frames_remaining == 0 || total_frames == 0) {
        return;
    }
    uint32_t remaining = *frames_remaining;
    uint32_t completed = total_frames > remaining ? total_frames - remaining : 0;
    for (size_t frame = 0; frame < frame_count; frame++) {
        if (remaining == 0) {
            break;
        }
        uint32_t gain_q15 = (uint32_t)(
            ((uint64_t)(completed + 1U) * 32768ULL) / (uint64_t)total_frames
        );
        if (gain_q15 > 32768U) {
            gain_q15 = 32768U;
        }
        for (size_t channel = 0; channel < PLAYBACK_SYNC_CHANNELS; channel++) {
            size_t index = (frame * PLAYBACK_SYNC_CHANNELS) + channel;
            stereo_frames[index] = (int16_t)(
                ((int32_t)stereo_frames[index] * (int32_t)gain_q15) / 32768
            );
        }
        completed++;
        remaining--;
    }
    *frames_remaining = remaining;
}
