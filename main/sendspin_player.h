#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "tater_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TATER_SENDSPIN_OUTPUT_STEREO = 0,
    TATER_SENDSPIN_OUTPUT_LEFT,
    TATER_SENDSPIN_OUTPUT_RIGHT,
    TATER_SENDSPIN_OUTPUT_MONO,
} tater_sendspin_output_channel_t;

typedef struct {
    bool active;
    uint32_t streams_started;
    uint32_t streams_completed;
    uint32_t audio_write_failures;
    uint32_t render_clock_failures;
    uint32_t output_starvations;
    uint32_t last_starvation_us;
    uint32_t max_starvation_us;
    uint32_t max_queued_frames;
    uint32_t time_sync_updates;
    uint32_t clock_error_us;
    uint32_t max_clock_error_us;
    uint16_t startup_headroom_ms;
    uint16_t output_delay_ms;
    int32_t fixed_output_delay_us;
    bool output_delay_adjustable;
    uint8_t sync_task_priority;
} tater_sendspin_stats_t;

esp_err_t tater_sendspin_init(const tater_config_t *config);

/*
 * Native Tater audio (interactive TTS, wake sounds, timers, and tones) owns
 * the speaker while this claim is held.  Sendspin is disconnected before the
 * claim returns so the two pipelines never write to I2S concurrently.
 */
bool tater_sendspin_claim_native_audio(uint32_t timeout_ms);
void tater_sendspin_release_native_audio(void);

void tater_sendspin_set_volume(uint8_t volume_percent);
bool tater_sendspin_set_output_channel_mode(const char *mode);
const char *tater_sendspin_output_channel_mode(void);
bool tater_sendspin_is_playing(void);
void tater_sendspin_stats_snapshot(tater_sendspin_stats_t *stats);

#ifdef __cplusplus
}
#endif
