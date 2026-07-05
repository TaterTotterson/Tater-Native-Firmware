#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t tater_playback_init(void);
esp_err_t tater_playback_play_url(const char *url);
esp_err_t tater_playback_play_url_local(const char *url);
esp_err_t tater_playback_play_wav_data_local(const uint8_t *data, size_t len, const char *label);
esp_err_t tater_playback_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent);
esp_err_t tater_playback_play_tone_local(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent);
void tater_playback_stop(void);
bool tater_playback_is_playing(void);
