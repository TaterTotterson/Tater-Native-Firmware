#include "leds.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "audio_i2s.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "native_settings.h"

static const char *TAG = "tater_leds";

static led_strip_handle_t s_strip;
static volatile tater_state_t s_state = TATER_STATE_DISCONNECTED;
static uint8_t s_brightness = 64;
static uint32_t s_state_epoch;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

static const rgb_t TATER_ORANGE = {255, 90, 31};
static const rgb_t TATER_BLUE = {30, 90, 255};
static const rgb_t TATER_WARM_WHITE = {255, 227, 181};
static const rgb_t TATER_RED = {255, 0, 24};
static const rgb_t TATER_GREEN = {57, 212, 160};
static const rgb_t TATER_VOICE_DEFAULT = {255, 90, 31};

static uint32_t s_render_epoch;
static uint32_t s_animation_tick;
static volatile uint8_t s_setup_feedback_mode;
static volatile uint8_t s_setup_feedback_value;
static volatile uint8_t s_setup_feedback_total;
static float s_thinking_levels[TATER_LED_COUNT];
static float s_speaking_radius = 1.25f;
static bool s_tool_forward = true;
static uint8_t s_tool_index;
static float s_listening_position = 6.0f;
static float s_listening_delay;
static int64_t s_listening_last_update_us;
static int64_t s_listening_last_valid_us;

static uint8_t scale(uint8_t value)
{
    return (uint8_t)(((uint16_t)value * s_brightness) / 255);
}

static uint8_t clamp_u8(float value)
{
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 255.0f) {
        return 255;
    }
    return (uint8_t)value;
}

static float clamp01(float value)
{
    if (value <= 0.0f) {
        return 0.0f;
    }
    if (value >= 1.0f) {
        return 1.0f;
    }
    return value;
}

static float triangle_wave(uint32_t tick, uint32_t period)
{
    if (period < 2) {
        return 1.0f;
    }
    uint32_t step = tick % period;
    uint32_t half = period / 2;
    if (half == 0) {
        return 1.0f;
    }
    if (step > half) {
        step = period - step;
    }
    return clamp01((float)step / (float)half);
}

static uint8_t apply_level(uint8_t value, uint8_t level)
{
    return (uint8_t)(((uint16_t)value * level) / 255);
}

static int map_led_index(int logical_index)
{
    static const uint8_t map[TATER_LED_COUNT] = {7, 8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6};
    if (logical_index < 0 || logical_index >= TATER_LED_COUNT) {
        return -1;
    }
    return map[logical_index];
}

static void set_pixel(int index, uint8_t r, uint8_t g, uint8_t b)
{
    int physical = map_led_index(index);
    if (!s_strip || physical < 0) {
        return;
    }
    led_strip_set_pixel(s_strip, physical, scale(r), scale(g), scale(b));
}

static void fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        set_pixel(i, r, g, b);
    }
}

static void set_color_level(int index, rgb_t color, float level)
{
    set_pixel(
        index,
        clamp_u8((float)color.r * level),
        clamp_u8((float)color.g * level),
        clamp_u8((float)color.b * level)
    );
}

static void paired_spinner(uint32_t tick, rgb_t color, bool fast)
{
    int head = (fast ? tick : tick / 2) % TATER_LED_COUNT;
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        uint8_t level = 0;
        if (i == head || i == (head + 6) % TATER_LED_COUNT) {
            level = 255;
        } else if (i == (head + 11) % TATER_LED_COUNT || i == (head + 5) % TATER_LED_COUNT) {
            level = 192;
        } else if (i == (head + 10) % TATER_LED_COUNT || i == (head + 4) % TATER_LED_COUNT) {
            level = 128;
        }
        set_pixel(i, apply_level(color.r, level), apply_level(color.g, level), apply_level(color.b, level));
    }
}

static void twinkle(uint32_t tick, rgb_t color)
{
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        uint8_t phase = (uint8_t)((i * 7 + tick * 3) % 31);
        float level = 0.04f;
        if (phase == 0) {
            level = 0.95f;
        } else if (phase == 1 || phase == 30) {
            level = 0.48f;
        } else if (phase == 2 || phase == 29) {
            level = 0.20f;
        }
        set_color_level(i, color, level);
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static bool parse_hex_color(const char *value, rgb_t *out)
{
    if (!value || !out) {
        return false;
    }
    const char *text = value[0] == '#' ? value + 1 : value;
    if (strlen(text) != 6) {
        return false;
    }
    int digits[6] = {0};
    for (int i = 0; i < 6; i++) {
        digits[i] = hex_digit(text[i]);
        if (digits[i] < 0) {
            return false;
        }
    }
    out->r = (uint8_t)((digits[0] << 4) | digits[1]);
    out->g = (uint8_t)((digits[2] << 4) | digits[3]);
    out->b = (uint8_t)((digits[4] << 4) | digits[5]);
    return true;
}

static rgb_t configured_voice_color(const tater_live_settings_t *settings)
{
    rgb_t color = TATER_VOICE_DEFAULT;
    if (settings) {
        parse_hex_color(settings->led_color, &color);
    }
    return color;
}

static bool animation_is(const char *value, const char *expected)
{
    return value && expected && strcasecmp(value, expected) == 0;
}

static void solid_color(rgb_t color)
{
    fill(color.r, color.g, color.b);
}

static void voice_pulse(uint32_t tick, rgb_t color)
{
    uint8_t step = tick % 24;
    uint8_t wave = step <= 12 ? step : 24 - step;
    float level = 0.10f + ((float)wave / 12.0f) * 0.82f;
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        set_color_level(i, color, level);
    }
}

static int ring_distance(int a, int b)
{
    int dist = a - b;
    if (dist < 0) {
        dist = -dist;
    }
    return dist > (TATER_LED_COUNT / 2) ? TATER_LED_COUNT - dist : dist;
}

static float wrap_position(float position)
{
    while (position >= (float)TATER_LED_COUNT) {
        position -= (float)TATER_LED_COUNT;
    }
    while (position < 0.0f) {
        position += (float)TATER_LED_COUNT;
    }
    return position;
}

static float ring_distance_f(float a, float b)
{
    float dist = a - b;
    if (dist < 0.0f) {
        dist = -dist;
    }
    return dist > ((float)TATER_LED_COUNT / 2.0f) ? (float)TATER_LED_COUNT - dist : dist;
}

static float delay_to_position(float delay)
{
    const float center_led = 6.0f;
    const float side_led_span = 3.5f;
    const float max_abs_delay = 1.2f;
    const float delay_deadband = 0.08f;

    if (delay > max_abs_delay) {
        delay = max_abs_delay;
    } else if (delay < -max_abs_delay) {
        delay = -max_abs_delay;
    }

    float normalized = delay / max_abs_delay;
    float magnitude = normalized < 0.0f ? -normalized : normalized;
    if (magnitude < delay_deadband) {
        normalized = 0.0f;
    } else {
        magnitude = (magnitude - delay_deadband) / (1.0f - delay_deadband);
        magnitude = 1.0f - ((1.0f - magnitude) * (1.0f - magnitude));
        normalized = normalized < 0.0f ? -magnitude : magnitude;
    }

    return wrap_position(center_led - (normalized * side_led_span));
}

static bool current_doa_delay(float *delay)
{
    tater_audio_doa_t doa = {0};
    if (!tater_audio_doa_snapshot(&doa)) {
        return false;
    }
    if (!doa.valid || doa.confidence < 1 || doa.age_ms > 500) {
        return false;
    }
    *delay = (float)doa.sample_delay;
    return true;
}

static void directional_listening(rgb_t color)
{
    const int fade_leds = 3;
    const float center_led = 6.0f;
    const float delay_filter_alpha = 0.22f;
    const float transition_duration = 0.25f;
    const int64_t now_us = esp_timer_get_time();

    float dt = 0.05f;
    if (s_listening_last_update_us > 0 && now_us >= s_listening_last_update_us) {
        dt = (float)(now_us - s_listening_last_update_us) / 1000000.0f;
        if (dt > 0.2f) {
            dt = 0.2f;
        }
    }
    s_listening_last_update_us = now_us;

    float delay = 0.0f;
    bool has_valid_doa = current_doa_delay(&delay);
    if (s_animation_tick == 0) {
        s_listening_delay = delay;
        s_listening_position = has_valid_doa ? delay_to_position(delay) : center_led;
        s_listening_last_valid_us = has_valid_doa ? now_us : 0;
        dt = 0.0f;
    }

    if (has_valid_doa) {
        s_listening_delay += (delay - s_listening_delay) * delay_filter_alpha;
        float target_position = delay_to_position(s_listening_delay);
        s_listening_last_valid_us = now_us;

        float diff = target_position - s_listening_position;
        if (diff > ((float)TATER_LED_COUNT / 2.0f)) {
            diff -= (float)TATER_LED_COUNT;
        } else if (diff < -((float)TATER_LED_COUNT / 2.0f)) {
            diff += (float)TATER_LED_COUNT;
        }
        if (diff > 0.01f || diff < -0.01f) {
            s_listening_position += (diff / transition_duration) * dt;
        } else {
            s_listening_position = target_position;
        }
    } else if (s_listening_last_valid_us == 0 || now_us - s_listening_last_valid_us > 1000000) {
        float diff = center_led - s_listening_position;
        if (diff > ((float)TATER_LED_COUNT / 2.0f)) {
            diff -= (float)TATER_LED_COUNT;
        } else if (diff < -((float)TATER_LED_COUNT / 2.0f)) {
            diff += (float)TATER_LED_COUNT;
        }
        s_listening_position += diff * 0.08f;
    }

    s_listening_position = wrap_position(s_listening_position);

    for (int i = 0; i < TATER_LED_COUNT; i++) {
        float dist = ring_distance_f((float)i, s_listening_position);
        float beam_level = 1.0f - (dist / ((float)fade_leds + 1.0f));
        if (beam_level < 0.0f) {
            beam_level = 0.0f;
        }
        beam_level *= 0.65f;

        float center_level = 1.0f - (dist / 1.20f);
        if (center_level < 0.0f) {
            center_level = 0.0f;
        }

        float color_level = 0.06f + beam_level;
        float r = (float)color.r * color_level + 255.0f * center_level;
        float g = (float)color.g * color_level + 240.0f * center_level;
        float b = (float)color.b * color_level + 170.0f * center_level;
        set_pixel(i, clamp_u8(r), clamp_u8(g), clamp_u8(b));
    }
}

static void thinking(uint32_t tick, rgb_t color)
{
    uint8_t breath_step = tick % 18;
    uint8_t breath = breath_step <= 9 ? breath_step : 18 - breath_step;

    for (int i = 0; i < TATER_LED_COUNT; i++) {
        float target = 0.025f + ((float)breath * 0.006f);

        uint8_t bit_phase = (uint8_t)((i * 7 + tick * 5) % 29);
        if (bit_phase == 0) {
            target += 0.86f;
        } else if (bit_phase == 1 || bit_phase == 28) {
            target += 0.46f;
        }

        uint8_t calc_phase = (uint8_t)((i * 5 + tick * 2) % 17);
        if (calc_phase == 0 || calc_phase == 8) {
            target += 0.50f;
        } else if (calc_phase == 1 || calc_phase == 9) {
            target += 0.24f;
        }

        if (target > 1.0f) {
            target = 1.0f;
        }

        float alpha = target > s_thinking_levels[i] ? 0.58f : 0.20f;
        s_thinking_levels[i] += (target - s_thinking_levels[i]) * alpha;

        float color_level = s_thinking_levels[i];
        float white_level = color_level > 0.55f ? (color_level - 0.55f) * 0.46f : 0.0f;
        float r = (float)color.r * color_level + 255.0f * white_level;
        float g = (float)color.g * color_level + 240.0f * white_level;
        float b = (float)color.b * color_level + 170.0f * white_level;
        set_pixel(i, clamp_u8(r), clamp_u8(g), clamp_u8(b));
    }
}

static void tool_call(uint32_t tick, rgb_t color)
{
    (void)tick;
    uint8_t lead_a = s_tool_index % 6;

    uint8_t lead_b = (uint8_t)((11 - lead_a) % TATER_LED_COUNT);
    uint8_t trail_a = s_tool_forward ? (uint8_t)((TATER_LED_COUNT + lead_a - 1) % TATER_LED_COUNT) : (uint8_t)((lead_a + 1) % TATER_LED_COUNT);
    uint8_t trail_b = s_tool_forward ? (uint8_t)((lead_b + 1) % TATER_LED_COUNT) : (uint8_t)((TATER_LED_COUNT + lead_b - 1) % TATER_LED_COUNT);
    uint8_t tail_a = s_tool_forward ? (uint8_t)((TATER_LED_COUNT + lead_a - 2) % TATER_LED_COUNT) : (uint8_t)((lead_a + 2) % TATER_LED_COUNT);
    uint8_t tail_b = s_tool_forward ? (uint8_t)((lead_b + 2) % TATER_LED_COUNT) : (uint8_t)((TATER_LED_COUNT + lead_b - 2) % TATER_LED_COUNT);

    for (int i = 0; i < TATER_LED_COUNT; i++) {
        uint8_t level = 0;
        if (i == lead_a || i == lead_b) {
            level = 255;
        } else if (i == trail_a || i == trail_b) {
            level = 192;
        } else if (i == tail_a || i == tail_b) {
            level = 128;
        }
        set_pixel(i, apply_level(color.r, level), apply_level(color.g, level), apply_level(color.b, level));
    }

    if (s_tool_forward) {
        if (s_tool_index >= 5) {
            s_tool_index = 4;
            s_tool_forward = false;
        } else {
            s_tool_index++;
        }
    } else if (s_tool_index <= 0) {
        s_tool_index = 1;
        s_tool_forward = true;
    } else {
        s_tool_index--;
    }
}

static void breathe(uint32_t tick, rgb_t color)
{
    float level = 0.08f + (triangle_wave(tick, 34) * 0.84f);
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        set_color_level(i, color, level);
    }
}

static void comet(uint32_t tick, rgb_t color, bool dual)
{
    int head = tick % TATER_LED_COUNT;
    int second_head = (head + (TATER_LED_COUNT / 2)) % TATER_LED_COUNT;
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        int dist = (head - i + TATER_LED_COUNT) % TATER_LED_COUNT;
        float level = dist == 0 ? 1.0f : dist == 1 ? 0.68f : dist == 2 ? 0.40f : dist == 3 ? 0.20f : dist == 4 ? 0.09f : 0.015f;
        if (dual) {
            int dist2 = (second_head - i + TATER_LED_COUNT) % TATER_LED_COUNT;
            float level2 = dist2 == 0 ? 1.0f : dist2 == 1 ? 0.68f : dist2 == 2 ? 0.40f : dist2 == 3 ? 0.20f : dist2 == 4 ? 0.09f : 0.015f;
            if (level2 > level) {
                level = level2;
            }
        }
        set_color_level(i, color, level);
    }
}

static void scanner(uint32_t tick, rgb_t color)
{
    const int period = (TATER_LED_COUNT - 1) * 2;
    int phase = tick % period;
    int head = phase < TATER_LED_COUNT ? phase : period - phase;
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        int dist = ring_distance(i, head);
        float level = dist == 0 ? 1.0f : dist == 1 ? 0.54f : dist == 2 ? 0.22f : 0.025f;
        set_color_level(i, color, level);
    }
}

static void ripple(uint32_t tick, rgb_t color)
{
    float radius = triangle_wave(tick, 24) * 5.75f;
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        float dist = (float)ring_distance(i, 6);
        float level = 1.0f - ((dist - radius) < 0.0f ? radius - dist : dist - radius) / 1.45f;
        level = 0.035f + (clamp01(level) * 0.88f);
        set_color_level(i, color, level);
    }
}

static void heartbeat(uint32_t tick, rgb_t color)
{
    uint32_t phase = tick % 34;
    float pulse = 0.0f;
    if (phase <= 4) {
        pulse = 1.0f - ((float)phase / 5.0f);
    } else if (phase >= 8 && phase <= 11) {
        pulse = 0.72f * (1.0f - ((float)(phase - 8) / 4.0f));
    }
    float level = 0.04f + (pulse * 0.92f);
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        float offset = (i % 2) == 0 ? 1.0f : 0.72f;
        set_color_level(i, color, level * offset);
    }
}

static void theater(uint32_t tick, rgb_t color)
{
    uint8_t phase = tick % 6;
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        uint8_t slot = (uint8_t)((i + phase) % 6);
        float level = slot == 0 ? 1.0f : slot == 1 ? 0.48f : slot == 5 ? 0.24f : 0.025f;
        set_color_level(i, color, level);
    }
}

static void wave(uint32_t tick, rgb_t color)
{
    float center = (float)((tick / 2) % TATER_LED_COUNT);
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        float dist = ring_distance_f((float)i, center);
        float level = 0.05f + (clamp01(1.0f - (dist / 4.2f)) * 0.84f);
        float cross = clamp01(1.0f - (ring_distance_f((float)i, wrap_position(center + 6.0f)) / 2.4f));
        level += cross * 0.16f;
        set_color_level(i, color, clamp01(level));
    }
}

static void shimmer(uint32_t tick, rgb_t color)
{
    float breath = 0.08f + (triangle_wave(tick, 28) * 0.18f);
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        uint32_t phase = (uint32_t)((i * 17 + tick * 5) % 37);
        float level = breath;
        if (phase == 0) {
            level = 1.0f;
        } else if (phase <= 3 || phase >= 34) {
            level = 0.46f;
        } else if ((phase + i) % 11 == 0) {
            level = 0.28f;
        }
        set_color_level(i, color, level);
    }
}

static void equalizer(uint32_t tick, rgb_t color)
{
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        uint32_t phase = (uint32_t)((i * 5 + tick * 3) % 20);
        if (phase > 10) {
            phase = 20 - phase;
        }
        float level = 0.05f + ((float)phase / 10.0f) * 0.72f;
        if (((i * 13 + tick) % 17) == 0) {
            level = 1.0f;
        }
        set_color_level(i, color, clamp01(level));
    }
}

static void replying(uint32_t tick, rgb_t color)
{
    int center = 6;
    float audio_level = tater_audio_speaker_level() * 5.5f;
    if (audio_level > 1.0f) {
        audio_level = 1.0f;
    }

    uint8_t breath_step = tick % 18;
    float breath = (float)(breath_step <= 9 ? breath_step : 18 - breath_step) / 9.0f;
    float motion_level = 0.10f + (breath * 0.16f);

    const tater_live_settings_t *settings = tater_live_settings_get();
    float volume_level = settings ? (float)settings->volume_percent / 100.0f : 0.8f;
    if (volume_level < 0.0f) {
        volume_level = 0.0f;
    } else if (volume_level > 1.0f) {
        volume_level = 1.0f;
    }

    float target_radius = 0.85f + ((audio_level + motion_level) * 3.15f) + (volume_level * 0.85f);
    if (target_radius > 5.5f) {
        target_radius = 5.5f;
    }
    float alpha = target_radius > s_speaking_radius ? 0.45f : 0.20f;
    s_speaking_radius += (target_radius - s_speaking_radius) * alpha;

    int sweep = (tick / 2) % TATER_LED_COUNT;

    for (int i = 0; i < TATER_LED_COUNT; i++) {
        int dist = ring_distance(i, center);
        float level = s_speaking_radius - (float)dist;
        if (level <= 0.0f) {
            set_pixel(i, 0, 0, 0);
            continue;
        }
        if (level > 1.0f) {
            level = 1.0f;
        }

        float ripple = 0.80f + (0.20f * (float)((tick + dist * 3) % 8) / 7.0f);
        float color_level = (0.12f + (level * 0.76f)) * ripple;
        int sweep_dist = ring_distance(i, sweep);
        if (sweep_dist <= 2) {
            color_level += 0.12f * (1.0f - ((float)sweep_dist / 3.0f));
        }
        float center_level = 1.0f - ((float)dist / 1.6f);
        if (center_level < 0.0f) {
            center_level = 0.0f;
        }
        center_level *= 0.52f;

        float r = (float)color.r * color_level + 255.0f * center_level;
        float g = (float)color.g * color_level + 240.0f * center_level;
        float b = (float)color.b * color_level + 170.0f * center_level;
        set_pixel(i, clamp_u8(r), clamp_u8(g), clamp_u8(b));
    }
}

static void render_voice_animation(const char *animation, const char *fallback, uint32_t tick, rgb_t color)
{
    const char *token = animation && animation[0] ? animation : fallback;
    if (animation_is(token, "directional")) {
        directional_listening(color);
    } else if (animation_is(token, "sparkle")) {
        thinking(tick, color);
    } else if (animation_is(token, "ping_pong")) {
        tool_call(tick, color);
    } else if (animation_is(token, "voice_ring")) {
        replying(tick, color);
    } else if (animation_is(token, "spinner")) {
        paired_spinner(tick, color, false);
    } else if (animation_is(token, "orbit")) {
        paired_spinner(tick, color, true);
    } else if (animation_is(token, "pulse")) {
        voice_pulse(tick, color);
    } else if (animation_is(token, "breathe")) {
        breathe(tick, color);
    } else if (animation_is(token, "comet")) {
        comet(tick, color, false);
    } else if (animation_is(token, "dual_comet")) {
        comet(tick, color, true);
    } else if (animation_is(token, "scanner")) {
        scanner(tick, color);
    } else if (animation_is(token, "ripple")) {
        ripple(tick, color);
    } else if (animation_is(token, "heartbeat")) {
        heartbeat(tick, color);
    } else if (animation_is(token, "theater")) {
        theater(tick, color);
    } else if (animation_is(token, "wave")) {
        wave(tick, color);
    } else if (animation_is(token, "shimmer")) {
        shimmer(tick, color);
    } else if (animation_is(token, "twinkle")) {
        twinkle(tick, color);
    } else if (animation_is(token, "equalizer")) {
        equalizer(tick, color);
    } else if (animation_is(token, "solid")) {
        solid_color(color);
    } else if (fallback && fallback[0] && !animation_is(token, fallback)) {
        render_voice_animation(fallback, "", tick, color);
    } else {
        solid_color(color);
    }
}

static void error_pulse(uint32_t tick, rgb_t color)
{
    uint8_t step = tick % 20;
    uint8_t brightness_step = step <= 10 ? step : 20 - step;
    uint8_t level = (uint8_t)(255 - (brightness_step * 22));
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        set_pixel(i, apply_level(color.r, level), apply_level(color.g, level), apply_level(color.b, level));
    }
}

static void setup_reset_clicks(uint8_t clicks, uint8_t required_clicks)
{
    if (required_clicks == 0) {
        required_clicks = 1;
    }
    if (clicks > required_clicks) {
        clicks = required_clicks;
    }

    fill(0, 0, 0);
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        uint8_t segment = (uint8_t)(((i + 1) * required_clicks + TATER_LED_COUNT - 1) / TATER_LED_COUNT);
        if (segment <= clicks) {
            set_pixel(i, TATER_ORANGE.r, TATER_ORANGE.g, TATER_ORANGE.b);
        } else {
            set_pixel(i, 20, 10, 0);
        }
    }
}

static void setup_reset_countdown(uint32_t tick, uint8_t remaining_steps, uint8_t total_steps)
{
    if (total_steps == 0) {
        total_steps = 1;
    }
    if (remaining_steps > total_steps) {
        remaining_steps = total_steps;
    }

    uint8_t lit = (uint8_t)(((uint16_t)remaining_steps * TATER_LED_COUNT + total_steps - 1) / total_steps);
    uint8_t pulse = (uint8_t)(110 + ((tick % 8) * 18));
    if (pulse > 255) {
        pulse = 255;
    }

    for (int i = 0; i < TATER_LED_COUNT; i++) {
        if (i < lit) {
            rgb_t color = remaining_steps <= (total_steps / 4) ? TATER_RED : TATER_ORANGE;
            set_pixel(i, apply_level(color.r, pulse), apply_level(color.g, pulse), apply_level(color.b, pulse));
        } else {
            set_pixel(i, 8, 0, 0);
        }
    }
}

static void setup_reset_success(uint32_t tick)
{
    uint8_t level = (tick % 2) == 0 ? 255 : 120;
    for (int i = 0; i < TATER_LED_COUNT; i++) {
        set_pixel(
            i,
            apply_level(TATER_GREEN.r, level),
            apply_level(TATER_GREEN.g, level),
            apply_level(TATER_GREEN.b, level)
        );
    }
}

static bool render_setup_reset_feedback(void)
{
    uint8_t mode = s_setup_feedback_mode;
    if (mode == 0) {
        return false;
    }

    uint8_t value = s_setup_feedback_value;
    uint8_t total = s_setup_feedback_total;
    if (mode == 1) {
        setup_reset_clicks(value, total);
    } else if (mode == 2) {
        setup_reset_countdown(s_animation_tick, value, total);
    } else {
        setup_reset_success(s_animation_tick);
    }
    return true;
}

static void render(void)
{
    if (s_render_epoch != s_state_epoch) {
        s_render_epoch = s_state_epoch;
        s_animation_tick = 0;
        s_speaking_radius = 1.25f;
        s_tool_forward = true;
        s_tool_index = 0;
        s_listening_position = 6.0f;
        s_listening_delay = 0.0f;
        s_listening_last_update_us = 0;
        s_listening_last_valid_us = 0;
        memset(s_thinking_levels, 0, sizeof(s_thinking_levels));
    }

    if (render_setup_reset_feedback()) {
        led_strip_refresh(s_strip);
        s_animation_tick++;
        return;
    }

    const tater_live_settings_t *settings = tater_live_settings_get();
    rgb_t voice_color = configured_voice_color(settings);

    switch (s_state) {
    case TATER_STATE_PROVISIONING:
        twinkle(s_animation_tick, TATER_WARM_WHITE);
        break;
    case TATER_STATE_IDLE:
        fill(0, 0, 0);
        break;
    case TATER_STATE_LISTENING:
        render_voice_animation(settings ? settings->led_listening_animation : "", "directional", s_animation_tick, voice_color);
        break;
    case TATER_STATE_THINKING:
        render_voice_animation(settings ? settings->led_thinking_animation : "", "sparkle", s_animation_tick, voice_color);
        break;
    case TATER_STATE_SPEAKING:
        render_voice_animation(settings ? settings->led_replying_animation : "", "voice_ring", s_animation_tick, voice_color);
        break;
    case TATER_STATE_TOOL_CALL:
        render_voice_animation(settings ? settings->led_tool_call_animation : "", "ping_pong", s_animation_tick, voice_color);
        break;
    case TATER_STATE_TIMER:
        render_voice_animation("heartbeat", "heartbeat", s_animation_tick, TATER_RED);
        break;
    case TATER_STATE_OTA:
        paired_spinner(s_animation_tick, TATER_BLUE, false);
        break;
    case TATER_STATE_ERROR:
        error_pulse(s_animation_tick, TATER_RED);
        break;
    case TATER_STATE_DISCONNECTED:
    default:
        twinkle(s_animation_tick, TATER_RED);
        break;
    }
    led_strip_refresh(s_strip);
    s_animation_tick++;
}

static void led_task(void *arg)
{
    while (true) {
        if (s_strip) {
            render();
        }
        uint32_t delay_ms = 80;
        if (s_state == TATER_STATE_SPEAKING) {
            delay_ms = 40;
        } else if (s_state == TATER_STATE_LISTENING) {
            delay_ms = 50;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t tater_leds_init(void)
{
    gpio_config_t power_config = {
        .pin_bit_mask = 1ULL << TATER_LED_POWER_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t power_err = gpio_config(&power_config);
    if (power_err != ESP_OK) {
        ESP_LOGE(TAG, "led power gpio init failed: %s", esp_err_to_name(power_err));
        return power_err;
    }
    gpio_set_level(TATER_LED_POWER_EN, 1);

    led_strip_config_t strip_config = {
        .strip_gpio_num = TATER_LED_PIN,
        .max_leds = TATER_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led strip init failed: %s", esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);
    xTaskCreatePinnedToCore(led_task, "tater_leds", 4096, NULL, 4, NULL, 0);
    return ESP_OK;
}

void tater_leds_set_state(tater_state_t state)
{
    if (s_state != state) {
        s_state = state;
        s_state_epoch++;
    }
}

void tater_leds_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
}

void tater_leds_show_setup_reset_clicks(uint8_t clicks, uint8_t required_clicks)
{
    s_setup_feedback_value = clicks;
    s_setup_feedback_total = required_clicks ? required_clicks : 1;
    s_setup_feedback_mode = 1;
}

void tater_leds_show_setup_reset_countdown(uint8_t remaining_steps, uint8_t total_steps)
{
    s_setup_feedback_value = remaining_steps;
    s_setup_feedback_total = total_steps ? total_steps : 1;
    s_setup_feedback_mode = 2;
}

void tater_leds_show_setup_reset_success(void)
{
    s_setup_feedback_value = 0;
    s_setup_feedback_total = 0;
    s_setup_feedback_mode = 3;
}

void tater_leds_clear_setup_reset_feedback(void)
{
    s_setup_feedback_mode = 0;
    s_setup_feedback_value = 0;
    s_setup_feedback_total = 0;
}
