#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct {
    char wake_engine[32];
    char wake_word[32];
    char wake_word_url[256];
    char wake_sensitivity[24];
    char wake_environment[32];
    float wake_threshold;
    uint8_t wake_sliding_window;
    bool capture_wake_audio;
    bool capture_close_misses;
    float close_miss_threshold;
    char trainer_app_url[128];
    bool wake_sound_enabled;
    char wake_sound[64];
    char wake_sound_url[192];
    bool aec_enabled;
    uint8_t aec_strength_percent;
    uint8_t aec_delay_ms;
    bool continued_chat;
    bool barge_in_enabled;
    uint8_t volume_percent;
    uint8_t led_brightness;
    char led_color[8];
    char led_listening_animation[32];
    char led_thinking_animation[32];
    char led_tool_call_animation[32];
    char led_replying_animation[32];
    char logging_level[16];
} tater_live_settings_t;

void tater_live_settings_init_defaults(void);
const tater_live_settings_t *tater_live_settings_get(void);
bool tater_live_settings_apply_json(const cJSON *payload);
void tater_live_settings_add_status(cJSON *payload);
