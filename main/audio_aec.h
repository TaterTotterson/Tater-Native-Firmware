#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool enabled;
    bool active;
    uint32_t processed_frames;
    uint32_t active_frames;
    uint32_t reference_frames;
    float last_mic_level;
    float last_reference_level;
    float last_speaker_level;
    float last_output_gain;
    uint8_t strength_percent;
    uint8_t delay_ms;
} tater_audio_aec_stats_t;

void tater_audio_aec_init(void);
void tater_audio_aec_note_speaker_frames(const int16_t *stereo_frames, size_t frame_count);
void tater_audio_aec_process_mic(int16_t *mono_frames, size_t frame_count);
bool tater_audio_aec_stats_snapshot(tater_audio_aec_stats_t *out);
