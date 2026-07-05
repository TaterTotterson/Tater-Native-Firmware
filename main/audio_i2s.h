#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int16_t sample_delay;
    uint8_t confidence;
    bool valid;
    uint32_t energy;
    uint32_t frame_counter;
    uint32_t age_ms;
} tater_audio_doa_t;

typedef enum {
    TATER_XMOS_UPDATE_IDLE = 0,
    TATER_XMOS_UPDATE_SKIPPED,
    TATER_XMOS_UPDATE_RUNNING,
    TATER_XMOS_UPDATE_COMPLETE,
    TATER_XMOS_UPDATE_ERROR,
} tater_audio_xmos_update_state_t;

typedef struct {
    bool version_valid;
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint8_t target_major;
    uint8_t target_minor;
    uint8_t target_patch;
    tater_audio_xmos_update_state_t update_state;
    bool update_attempted;
    bool update_required;
    uint8_t progress_percent;
    uint8_t dfu_state;
    uint8_t dfu_status;
} tater_audio_xmos_status_t;

esp_err_t tater_audio_i2s_init(void);
void tater_audio_i2s_start_task(void);
esp_err_t tater_audio_speaker_begin(void);
esp_err_t tater_audio_write_speaker_frames(const int16_t *stereo_frames, size_t frame_count);
esp_err_t tater_audio_speaker_end(void);
bool tater_audio_speaker_ready(void);
float tater_audio_speaker_level(void);
bool tater_audio_doa_snapshot(tater_audio_doa_t *out);
bool tater_audio_xmos_status_snapshot(tater_audio_xmos_status_t *out);
