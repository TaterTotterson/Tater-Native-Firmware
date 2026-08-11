#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tater_config.h"

typedef enum {
    TATER_STATE_DISCONNECTED = 0,
    TATER_STATE_IDLE,
    TATER_STATE_PROVISIONING,
    TATER_STATE_LISTENING,
    TATER_STATE_THINKING,
    TATER_STATE_SPEAKING,
    TATER_STATE_TOOL_CALL,
    TATER_STATE_TIMER,
    TATER_STATE_OTA,
    TATER_STATE_ERROR,
} tater_state_t;

typedef void (*tater_state_callback_t)(tater_state_t state, const char *detail);
typedef void (*tater_play_url_callback_t)(const char *url, tater_state_t visual_state);
typedef void (*tater_play_tone_callback_t)(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent);
typedef void (*tater_ota_url_callback_t)(const char *url);

void tater_protocol_init(
    const tater_config_t *config,
    tater_state_callback_t state_cb,
    tater_play_url_callback_t play_url_cb,
    tater_play_tone_callback_t play_tone_cb,
    tater_ota_url_callback_t ota_url_cb
);
void tater_protocol_start(void);
bool tater_protocol_is_connected(void);
bool tater_protocol_voice_active(void);
bool tater_protocol_can_start_local_wake(void);
bool tater_protocol_timer_is_active(void);
bool tater_protocol_timer_is_ringing(void);
void tater_protocol_timer_stop_from_device(void);
bool tater_protocol_can_detect_timer_stop(void);
const char *tater_protocol_device_id(void);
const char *tater_protocol_device_name(void);
const char *tater_protocol_room(void);
const char *tater_protocol_server_url(void);
const char *tater_protocol_token(void);
void tater_protocol_send_status(const char *state);
void tater_protocol_start_voice(const char *wake_word, const char *source);
void tater_protocol_start_voice_with_conversation(const char *wake_word, const char *source, const char *conversation_id);
void tater_protocol_stop_voice(bool abort);
void tater_protocol_send_audio(const int16_t *pcm, size_t sample_count);
uint32_t tater_protocol_send_wake_verification(
    uint32_t request_id,
    const int16_t *pcm,
    size_t sample_count,
    bool enforce
);
void tater_protocol_send_log(const char *level, const char *message);
void tater_protocol_send_playback_finished(void);
void tater_protocol_send_playback_finished_status(bool ok, bool allow_reopen);
void tater_protocol_send_audio_scene_finished(const char *scene_id, bool ok);
void tater_protocol_send_audio_overlay_started(const char *overlay_id);
void tater_protocol_send_audio_overlay_finished(const char *overlay_id, bool ok);
void tater_protocol_start_media_session_visual(bool tool_playback);
void tater_protocol_send_media_session_started(
    const char *session_id,
    const char *group_id,
    const char *channel,
    int64_t scheduled_start_us,
    int64_t actual_start_us
);
void tater_protocol_send_media_session_finished(
    const char *session_id,
    bool ok,
    bool complete_visual_state
);
void tater_protocol_send_media_session_ready(
    const char *session_id,
    const char *group_id,
    const char *reply_to,
    bool ok,
    uint32_t buffered_frames
);
void tater_protocol_send_media_session_playhead(
    const char *session_id,
    const char *group_id,
    const char *channel,
    uint64_t source_frames,
    uint64_t rendered_frames,
    uint64_t output_frames,
    uint32_t output_latency_frames,
    uint32_t buffered_frames,
    int64_t satellite_time_us,
    int64_t scheduled_start_us,
    int32_t correction_frames,
    bool rebuffering,
    uint32_t underrun_events,
    uint32_t rejoin_count,
    uint64_t rejoin_frames
);
void tater_protocol_send_ota_status(const char *status, int progress, const char *message);
