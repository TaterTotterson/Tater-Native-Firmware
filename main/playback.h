#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "playback_mix.h"

#define TATER_PLAYBACK_SCENE_ID_MAX 64
#define TATER_PLAYBACK_MEDIA_SESSION_ID_MAX 64
#define TATER_PLAYBACK_MEDIA_GROUP_ID_MAX 64
#define TATER_PLAYBACK_REQUEST_ID_MAX 64
#define TATER_PLAYBACK_OVERLAY_ID_MAX 64

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

typedef struct {
    const char *session_id;
    const char *group_id;
    const char *prepare_reply_to;
    const char *url;
    uint8_t volume_percent;
    uint32_t start_position_ms;
    tater_playback_channel_t channel;
    bool loop;
    bool prepare;
    bool complete_visual_state;
    bool tool_visual_state;
} tater_playback_media_session_t;

typedef struct {
    const char *overlay_id;
    const char *foreground_url;
    uint8_t foreground_volume_percent;
    uint8_t ducking_target_percent;
    uint16_t ducking_attack_ms;
    uint16_t ducking_release_ms;
    int64_t start_at_us;
} tater_playback_overlay_t;

esp_err_t tater_playback_init(void);
esp_err_t tater_playback_play_url(const char *url);
esp_err_t tater_playback_play_url_local(const char *url);
esp_err_t tater_playback_play_scene(const tater_playback_scene_t *scene);
esp_err_t tater_playback_start_media_session(const tater_playback_media_session_t *session);
esp_err_t tater_playback_commit_media_session(const char *session_id, int64_t start_at_us);
esp_err_t tater_playback_adjust_media_session(
    const char *session_id,
    int32_t correction_frames,
    const char *mode,
    uint32_t settle_ms
);
esp_err_t tater_playback_set_media_session_volume(const char *session_id, uint8_t volume_percent);
esp_err_t tater_playback_play_overlay(const tater_playback_overlay_t *overlay);
esp_err_t tater_playback_play_wav_data_local(const uint8_t *data, size_t len, const char *label);
esp_err_t tater_playback_play_wav_data_owned_local(uint8_t *data, size_t len, const char *label);
esp_err_t tater_playback_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent);
esp_err_t tater_playback_play_tone_local(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent);
void tater_playback_stop(void);
bool tater_playback_is_playing(void);
bool tater_playback_media_session_active(void);
