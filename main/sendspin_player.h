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

#ifdef __cplusplus
}
#endif
