#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t background_gain_q16;
    int32_t target_gain_q16;
    uint32_t ramp_frames_remaining;
} tater_playback_mix_state_t;

typedef enum {
    TATER_PLAYBACK_CHANNEL_STEREO = 0,
    TATER_PLAYBACK_CHANNEL_LEFT,
    TATER_PLAYBACK_CHANNEL_RIGHT,
    TATER_PLAYBACK_CHANNEL_MONO,
} tater_playback_channel_t;

void tater_playback_mix_init(tater_playback_mix_state_t *state, uint8_t background_percent);
void tater_playback_mix_set_background(
    tater_playback_mix_state_t *state,
    uint8_t background_percent,
    uint32_t ramp_frames
);
void tater_playback_mix_frames(
    tater_playback_mix_state_t *state,
    const int16_t *foreground_stereo,
    const int16_t *background_stereo,
    int16_t *output_stereo,
    size_t frame_count
);
void tater_playback_route_channel(
    tater_playback_channel_t channel,
    int16_t *stereo_frames,
    size_t frame_count
);
