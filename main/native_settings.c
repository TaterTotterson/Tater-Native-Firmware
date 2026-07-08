#include "native_settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "leds.h"

static const char *TAG = "tater_settings";

static tater_live_settings_t s_settings;

static void strlcpy_or_empty(char *dst, const char *src, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

static bool json_bool(const cJSON *value, bool fallback)
{
    if (cJSON_IsBool(value)) {
        return cJSON_IsTrue(value);
    }
    if (cJSON_IsNumber(value)) {
        return value->valuedouble != 0.0;
    }
    if (cJSON_IsString(value) && value->valuestring) {
        if (strcasecmp(value->valuestring, "true") == 0 || strcmp(value->valuestring, "1") == 0 ||
            strcasecmp(value->valuestring, "yes") == 0 || strcasecmp(value->valuestring, "on") == 0 ||
            strcasecmp(value->valuestring, "enabled") == 0) {
            return true;
        }
        if (strcasecmp(value->valuestring, "false") == 0 || strcmp(value->valuestring, "0") == 0 ||
            strcasecmp(value->valuestring, "no") == 0 || strcasecmp(value->valuestring, "off") == 0 ||
            strcasecmp(value->valuestring, "disabled") == 0) {
            return false;
        }
    }
    return fallback;
}

static float json_float_range(const cJSON *value, float fallback, float min_value, float max_value)
{
    float out = fallback;
    if (cJSON_IsNumber(value)) {
        out = (float)value->valuedouble;
    } else if (cJSON_IsString(value) && value->valuestring) {
        char *end = NULL;
        out = strtof(value->valuestring, &end);
        if (end == value->valuestring) {
            out = fallback;
        }
    }
    if (!isfinite(out)) {
        out = fallback;
    }
    if (out < min_value) {
        out = min_value;
    }
    if (out > max_value) {
        out = max_value;
    }
    return out;
}

static uint8_t json_u8_range(const cJSON *value, uint8_t fallback, uint8_t min_value, uint8_t max_value)
{
    float out = json_float_range(value, fallback, min_value, max_value);
    return (uint8_t)(out + 0.5f);
}

void tater_live_settings_init_defaults(void)
{
    strlcpy_or_empty(s_settings.wake_engine, "micro_wake_word", sizeof(s_settings.wake_engine));
    strlcpy_or_empty(s_settings.wake_word, "hey_tater", sizeof(s_settings.wake_word));
    strlcpy_or_empty(s_settings.wake_word_url, "", sizeof(s_settings.wake_word_url));
    strlcpy_or_empty(s_settings.wake_sensitivity, "normal", sizeof(s_settings.wake_sensitivity));
    strlcpy_or_empty(s_settings.wake_environment, "balanced", sizeof(s_settings.wake_environment));
    s_settings.wake_threshold = 0.97f;
    s_settings.wake_sliding_window = 5;
    s_settings.capture_wake_audio = false;
    s_settings.capture_close_misses = false;
    s_settings.close_miss_threshold = 0.78f;
    strlcpy_or_empty(s_settings.trainer_app_url, "http://trainer.local:8789", sizeof(s_settings.trainer_app_url));
    s_settings.wake_sound_enabled = false;
    strlcpy_or_empty(s_settings.wake_sound, "no_sound", sizeof(s_settings.wake_sound));
    strlcpy_or_empty(s_settings.wake_sound_url, "", sizeof(s_settings.wake_sound_url));
    s_settings.aec_enabled = false;
    s_settings.aec_strength_percent = 70;
    s_settings.aec_delay_ms = 85;
    s_settings.continued_chat = true;
    s_settings.barge_in_enabled = false;
    s_settings.volume_percent = 80;
    s_settings.led_brightness = 64;
    strlcpy_or_empty(s_settings.led_color, "#ff5a1f", sizeof(s_settings.led_color));
    strlcpy_or_empty(s_settings.led_listening_animation, "directional", sizeof(s_settings.led_listening_animation));
    strlcpy_or_empty(s_settings.led_thinking_animation, "sparkle", sizeof(s_settings.led_thinking_animation));
    strlcpy_or_empty(s_settings.led_tool_call_animation, "ping_pong", sizeof(s_settings.led_tool_call_animation));
    strlcpy_or_empty(s_settings.led_replying_animation, "voice_ring", sizeof(s_settings.led_replying_animation));
    strlcpy_or_empty(s_settings.logging_level, "info", sizeof(s_settings.logging_level));
    tater_leds_set_brightness(s_settings.led_brightness);
}

const tater_live_settings_t *tater_live_settings_get(void)
{
    return &s_settings;
}

bool tater_live_settings_apply_json(const cJSON *payload)
{
    if (!cJSON_IsObject(payload)) {
        return false;
    }

    const cJSON *wake_engine = cJSON_GetObjectItem(payload, "wake_engine");
    const cJSON *wake_word = cJSON_GetObjectItem(payload, "wake_word");
    const cJSON *wake_word_url = cJSON_GetObjectItem(payload, "wake_word_url");
    const cJSON *wake_sensitivity = cJSON_GetObjectItem(payload, "wake_sensitivity");
    const cJSON *wake_environment = cJSON_GetObjectItem(payload, "wake_environment");
    const cJSON *wake_threshold = cJSON_GetObjectItem(payload, "wake_threshold");
    const cJSON *wake_sliding_window = cJSON_GetObjectItem(payload, "wake_sliding_window");
    const cJSON *capture_wake_audio = cJSON_GetObjectItem(payload, "capture_wake_audio");
    const cJSON *capture_close_misses = cJSON_GetObjectItem(payload, "capture_close_misses");
    const cJSON *close_miss_threshold = cJSON_GetObjectItem(payload, "close_miss_threshold");
    const cJSON *trainer_app_url = cJSON_GetObjectItem(payload, "trainer_app_url");
    const cJSON *wake_sound_enabled = cJSON_GetObjectItem(payload, "wake_sound_enabled");
    const cJSON *wake_sound = cJSON_GetObjectItem(payload, "wake_sound");
    const cJSON *wake_sound_url = cJSON_GetObjectItem(payload, "wake_sound_url");
    const cJSON *aec_enabled = cJSON_GetObjectItem(payload, "aec_enabled");
    const cJSON *aec_strength_percent = cJSON_GetObjectItem(payload, "aec_strength_percent");
    const cJSON *aec_delay_ms = cJSON_GetObjectItem(payload, "aec_delay_ms");
    const cJSON *continued_chat = cJSON_GetObjectItem(payload, "continued_chat");
    const cJSON *barge_in_enabled = cJSON_GetObjectItem(payload, "barge_in_enabled");
    const cJSON *volume_percent = cJSON_GetObjectItem(payload, "volume_percent");
    const cJSON *led_brightness = cJSON_GetObjectItem(payload, "led_brightness");
    const cJSON *led_color = cJSON_GetObjectItem(payload, "led_color");
    const cJSON *led_listening_animation = cJSON_GetObjectItem(payload, "led_listening_animation");
    const cJSON *led_thinking_animation = cJSON_GetObjectItem(payload, "led_thinking_animation");
    const cJSON *led_tool_call_animation = cJSON_GetObjectItem(payload, "led_tool_call_animation");
    const cJSON *led_replying_animation = cJSON_GetObjectItem(payload, "led_replying_animation");
    const cJSON *logging_level = cJSON_GetObjectItem(payload, "logging_level");

    if (cJSON_IsString(wake_engine) && wake_engine->valuestring && wake_engine->valuestring[0]) {
        strlcpy_or_empty(s_settings.wake_engine, wake_engine->valuestring, sizeof(s_settings.wake_engine));
    }
    if (cJSON_IsString(wake_word) && wake_word->valuestring && wake_word->valuestring[0]) {
        strlcpy_or_empty(s_settings.wake_word, wake_word->valuestring, sizeof(s_settings.wake_word));
    }
    if (cJSON_IsString(wake_word_url) && wake_word_url->valuestring) {
        strlcpy_or_empty(s_settings.wake_word_url, wake_word_url->valuestring, sizeof(s_settings.wake_word_url));
    }
    if (cJSON_IsString(wake_sensitivity) && wake_sensitivity->valuestring && wake_sensitivity->valuestring[0]) {
        strlcpy_or_empty(s_settings.wake_sensitivity, wake_sensitivity->valuestring, sizeof(s_settings.wake_sensitivity));
    }
    if (cJSON_IsString(wake_environment) && wake_environment->valuestring && wake_environment->valuestring[0]) {
        strlcpy_or_empty(s_settings.wake_environment, wake_environment->valuestring, sizeof(s_settings.wake_environment));
    }
    s_settings.wake_threshold = json_float_range(wake_threshold, s_settings.wake_threshold, 0.01f, 0.99f);
    s_settings.wake_sliding_window = json_u8_range(wake_sliding_window, s_settings.wake_sliding_window, 1, 10);
    s_settings.capture_wake_audio = json_bool(capture_wake_audio, s_settings.capture_wake_audio);
    s_settings.capture_close_misses = json_bool(capture_close_misses, s_settings.capture_close_misses);
    s_settings.close_miss_threshold = json_float_range(close_miss_threshold, s_settings.close_miss_threshold, 0.01f, 0.99f);
    if (cJSON_IsString(trainer_app_url) && trainer_app_url->valuestring) {
        strlcpy_or_empty(s_settings.trainer_app_url, trainer_app_url->valuestring, sizeof(s_settings.trainer_app_url));
    }
    s_settings.wake_sound_enabled = json_bool(wake_sound_enabled, s_settings.wake_sound_enabled);
    if (cJSON_IsString(wake_sound) && wake_sound->valuestring && wake_sound->valuestring[0]) {
        strlcpy_or_empty(s_settings.wake_sound, wake_sound->valuestring, sizeof(s_settings.wake_sound));
    }
    if (cJSON_IsString(wake_sound_url) && wake_sound_url->valuestring) {
        strlcpy_or_empty(s_settings.wake_sound_url, wake_sound_url->valuestring, sizeof(s_settings.wake_sound_url));
    }
    s_settings.aec_enabled = json_bool(aec_enabled, s_settings.aec_enabled);
    s_settings.aec_strength_percent = json_u8_range(aec_strength_percent, s_settings.aec_strength_percent, 0, 100);
    s_settings.aec_delay_ms = json_u8_range(aec_delay_ms, s_settings.aec_delay_ms, 0, 220);
    s_settings.continued_chat = json_bool(continued_chat, s_settings.continued_chat);
    s_settings.barge_in_enabled = json_bool(barge_in_enabled, s_settings.barge_in_enabled);
    s_settings.volume_percent = json_u8_range(volume_percent, s_settings.volume_percent, 0, 100);
    s_settings.led_brightness = json_u8_range(led_brightness, s_settings.led_brightness, 0, 255);
    if (cJSON_IsString(led_color) && led_color->valuestring && led_color->valuestring[0]) {
        strlcpy_or_empty(s_settings.led_color, led_color->valuestring, sizeof(s_settings.led_color));
    }
    if (cJSON_IsString(led_listening_animation) && led_listening_animation->valuestring && led_listening_animation->valuestring[0]) {
        strlcpy_or_empty(s_settings.led_listening_animation, led_listening_animation->valuestring, sizeof(s_settings.led_listening_animation));
    }
    if (cJSON_IsString(led_thinking_animation) && led_thinking_animation->valuestring && led_thinking_animation->valuestring[0]) {
        strlcpy_or_empty(s_settings.led_thinking_animation, led_thinking_animation->valuestring, sizeof(s_settings.led_thinking_animation));
    }
    if (cJSON_IsString(led_tool_call_animation) && led_tool_call_animation->valuestring && led_tool_call_animation->valuestring[0]) {
        strlcpy_or_empty(s_settings.led_tool_call_animation, led_tool_call_animation->valuestring, sizeof(s_settings.led_tool_call_animation));
    }
    if (cJSON_IsString(led_replying_animation) && led_replying_animation->valuestring && led_replying_animation->valuestring[0]) {
        strlcpy_or_empty(s_settings.led_replying_animation, led_replying_animation->valuestring, sizeof(s_settings.led_replying_animation));
    }
    tater_leds_set_brightness(s_settings.led_brightness);
    if (cJSON_IsString(logging_level) && logging_level->valuestring && logging_level->valuestring[0]) {
        strlcpy_or_empty(s_settings.logging_level, logging_level->valuestring, sizeof(s_settings.logging_level));
    }

    ESP_LOGI(
        TAG,
        "live settings applied wake_engine=%s wake_word=%s wake_word_url=%s sensitivity=%s environment=%s threshold=%.2f window=%u capture_wake=%d capture_close=%d close_threshold=%.2f wake_sound=%d/%s aec=%d/%u/%ums continued_chat=%d barge_in=%d volume=%u led=%u color=%s animations=%s/%s/%s/%s logging=%s",
        s_settings.wake_engine,
        s_settings.wake_word,
        s_settings.wake_word_url,
        s_settings.wake_sensitivity,
        s_settings.wake_environment,
        (double)s_settings.wake_threshold,
        s_settings.wake_sliding_window,
        s_settings.capture_wake_audio,
        s_settings.capture_close_misses,
        (double)s_settings.close_miss_threshold,
        s_settings.wake_sound_enabled,
        s_settings.wake_sound,
        s_settings.aec_enabled,
        s_settings.aec_strength_percent,
        s_settings.aec_delay_ms,
        s_settings.continued_chat,
        s_settings.barge_in_enabled,
        s_settings.volume_percent,
        s_settings.led_brightness,
        s_settings.led_color,
        s_settings.led_listening_animation,
        s_settings.led_thinking_animation,
        s_settings.led_tool_call_animation,
        s_settings.led_replying_animation,
        s_settings.logging_level
    );
    return true;
}

void tater_live_settings_add_status(cJSON *payload)
{
    if (!cJSON_IsObject(payload)) {
        return;
    }
    cJSON *settings = cJSON_CreateObject();
    cJSON_AddStringToObject(settings, "wake_engine", s_settings.wake_engine);
    cJSON_AddStringToObject(settings, "wake_word", s_settings.wake_word);
    cJSON_AddStringToObject(settings, "wake_word_url", s_settings.wake_word_url);
    cJSON_AddStringToObject(settings, "wake_sensitivity", s_settings.wake_sensitivity);
    cJSON_AddStringToObject(settings, "wake_environment", s_settings.wake_environment);
    cJSON_AddNumberToObject(settings, "wake_threshold", s_settings.wake_threshold);
    cJSON_AddNumberToObject(settings, "wake_sliding_window", s_settings.wake_sliding_window);
    cJSON_AddBoolToObject(settings, "capture_wake_audio", s_settings.capture_wake_audio);
    cJSON_AddBoolToObject(settings, "capture_close_misses", s_settings.capture_close_misses);
    cJSON_AddNumberToObject(settings, "close_miss_threshold", s_settings.close_miss_threshold);
    cJSON_AddStringToObject(settings, "trainer_app_url", s_settings.trainer_app_url);
    cJSON_AddBoolToObject(settings, "wake_sound_enabled", s_settings.wake_sound_enabled);
    cJSON_AddStringToObject(settings, "wake_sound", s_settings.wake_sound);
    cJSON_AddStringToObject(settings, "wake_sound_url", s_settings.wake_sound_url);
    cJSON_AddBoolToObject(settings, "aec_enabled", s_settings.aec_enabled);
    cJSON_AddNumberToObject(settings, "aec_strength_percent", s_settings.aec_strength_percent);
    cJSON_AddNumberToObject(settings, "aec_delay_ms", s_settings.aec_delay_ms);
    cJSON_AddBoolToObject(settings, "continued_chat", s_settings.continued_chat);
    cJSON_AddBoolToObject(settings, "barge_in_enabled", s_settings.barge_in_enabled);
    cJSON_AddNumberToObject(settings, "volume_percent", s_settings.volume_percent);
    cJSON_AddNumberToObject(settings, "led_brightness", s_settings.led_brightness);
    cJSON_AddStringToObject(settings, "led_color", s_settings.led_color);
    cJSON_AddStringToObject(settings, "led_listening_animation", s_settings.led_listening_animation);
    cJSON_AddStringToObject(settings, "led_thinking_animation", s_settings.led_thinking_animation);
    cJSON_AddStringToObject(settings, "led_tool_call_animation", s_settings.led_tool_call_animation);
    cJSON_AddStringToObject(settings, "led_replying_animation", s_settings.led_replying_animation);
    cJSON_AddStringToObject(settings, "logging_level", s_settings.logging_level);
    cJSON_AddItemToObject(payload, "live_settings", settings);
}
