#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t pending_frames;
    uint32_t step_interval_output_frames;
    uint32_t frames_until_step;
} tater_playback_sync_slew_t;

void tater_playback_sync_slew_init(tater_playback_sync_slew_t *state);

void tater_playback_sync_slew_queue(
    tater_playback_sync_slew_t *state,
    int32_t correction_frames,
    uint32_t settle_output_frames,
    int32_t max_pending_frames
);

int32_t tater_playback_sync_slew_next_step(
    tater_playback_sync_slew_t *state,
    uint32_t output_frames
);

void tater_playback_sync_slew_restore_step(
    tater_playback_sync_slew_t *state,
    int32_t correction_step
);

/*
 * Resample a very small input-rate difference into a fixed-size hardware
 * output block. This lets the media clock advance or retard one source frame
 * at a time without an audible dropped or repeated frame.
 */
void tater_playback_sync_resample_stereo(
    const int16_t *input,
    size_t input_frames,
    int16_t *output,
    size_t output_frames
);

/* Apply a linear fade after an underrun timeline rejoin. */
void tater_playback_sync_fade_in(
    int16_t *stereo_frames,
    size_t frame_count,
    uint32_t *frames_remaining,
    uint32_t total_frames
);
