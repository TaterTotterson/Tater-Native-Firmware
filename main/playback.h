#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "playback_mix.h"

#define TATER_PLAYBACK_SCENE_ID_MAX 64

typedef struct {
    const char *scene_id;
    const char *foreground_url;
    const char *background_url;
    uint8_t foreground_volume_percent;
    uint8_t background_volume_percent;
    uint8_t ducking_target_percent;
    uint16_t ducking_attack_ms;
    uint16_t ducking_release_ms;
    uint16_t background_fade_out_ms;
    bool background_loop;
} tater_playback_scene_t;

esp_err_t tater_playback_init(void);
esp_err_t tater_playback_play_url(const char *url);
esp_err_t tater_playback_play_url_local(const char *url);
esp_err_t tater_playback_play_scene(const tater_playback_scene_t *scene);
esp_err_t tater_playback_play_wav_data_local(const uint8_t *data, size_t len, const char *label);
esp_err_t tater_playback_play_wav_data_owned_local(uint8_t *data, size_t len, const char *label);
esp_err_t tater_playback_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent);
esp_err_t tater_playback_play_tone_local(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent);
void tater_playback_stop(void);
bool tater_playback_is_playing(void);
