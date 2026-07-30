#include "playback_mix.h"

#include <limits.h>

#define TATER_PLAYBACK_GAIN_ONE_Q16 65536

static int32_t percent_to_q16(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return ((int32_t)percent * TATER_PLAYBACK_GAIN_ONE_Q16) / 100;
}

static int16_t saturate_s16(int32_t sample)
{
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static void advance_background_gain(tater_playback_mix_state_t *state)
{
    if (!state || state->ramp_frames_remaining == 0) {
        return;
    }

    int32_t delta = state->target_gain_q16 - state->background_gain_q16;
    int32_t step = delta / (int32_t)state->ramp_frames_remaining;
    if (step == 0 && delta != 0) {
        step = delta > 0 ? 1 : -1;
    }
    state->background_gain_q16 += step;
    state->ramp_frames_remaining--;
    if (state->ramp_frames_remaining == 0) {
        state->background_gain_q16 = state->target_gain_q16;
    }
}

void tater_playback_mix_init(tater_playback_mix_state_t *state, uint8_t background_percent)
{
    if (!state) {
        return;
    }
    state->background_gain_q16 = percent_to_q16(background_percent);
    state->target_gain_q16 = state->background_gain_q16;
    state->ramp_frames_remaining = 0;
}

void tater_playback_mix_set_background(
    tater_playback_mix_state_t *state,
    uint8_t background_percent,
    uint32_t ramp_frames
)
{
    if (!state) {
        return;
    }
    state->target_gain_q16 = percent_to_q16(background_percent);
    state->ramp_frames_remaining = ramp_frames;
    if (ramp_frames == 0) {
        state->background_gain_q16 = state->target_gain_q16;
    }
}

void tater_playback_mix_frames(
    tater_playback_mix_state_t *state,
    const int16_t *foreground_stereo,
    const int16_t *background_stereo,
    int16_t *output_stereo,
    size_t frame_count
)
{
    if (!state || !output_stereo) {
        return;
    }

    for (size_t frame = 0; frame < frame_count; frame++) {
        advance_background_gain(state);
        for (size_t channel = 0; channel < 2; channel++) {
            size_t index = (frame * 2) + channel;
            int32_t foreground = foreground_stereo ? foreground_stereo[index] : 0;
            int32_t background = background_stereo ? background_stereo[index] : 0;
            int32_t mixed = foreground
                + (int32_t)(((int64_t)background * state->background_gain_q16)
                    / TATER_PLAYBACK_GAIN_ONE_Q16);
            output_stereo[index] = saturate_s16(mixed);
        }
    }
}

void tater_playback_route_channel(
    tater_playback_channel_t channel,
    int16_t *stereo_frames,
    size_t frame_count
)
{
    if (!stereo_frames || channel == TATER_PLAYBACK_CHANNEL_STEREO) {
        return;
    }

    for (size_t frame = 0; frame < frame_count; frame++) {
        size_t index = frame * 2;
        int16_t left = stereo_frames[index];
        int16_t right = stereo_frames[index + 1];
        int16_t routed = left;
        if (channel == TATER_PLAYBACK_CHANNEL_RIGHT) {
            routed = right;
        } else if (channel == TATER_PLAYBACK_CHANNEL_MONO) {
            routed = (int16_t)(((int32_t)left + (int32_t)right) / 2);
        }
        stereo_frames[index] = routed;
        stereo_frames[index + 1] = routed;
    }
}
