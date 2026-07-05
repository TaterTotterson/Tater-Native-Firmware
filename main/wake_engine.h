#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tater_wake_engine_init(void);
bool tater_wake_engine_ready(void);
void tater_wake_engine_reset(void);
void tater_wake_engine_note_audio(const int16_t *pcm, size_t sample_count);
void tater_wake_engine_process(const int16_t *pcm, size_t sample_count);
void tater_wake_engine_add_status(cJSON *payload);

#ifdef __cplusplus
}
#endif
