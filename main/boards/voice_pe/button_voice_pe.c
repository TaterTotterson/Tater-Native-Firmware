#include "button.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "audio_i2s.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "native_settings.h"
#include "ota_update.h"
#include "playback.h"
#include "tater_config.h"
#include "tater_protocol.h"
#include "wake_sound_assets.h"

static const char *TAG = "tater_button";

#define BUTTON_POLL_MS 20
#define BUTTON_DEBOUNCE_TICKS 3
#define INTERCOM_START_TICKS (600 / BUTTON_POLL_MS)
#define SETUP_RESET_CLICK_COUNT 5
#define SETUP_RESET_CLICK_WINDOW_TICKS (3000 / BUTTON_POLL_MS)
#define SETUP_RESET_ARMED_TICKS (5000 / BUTTON_POLL_MS)
#define SETUP_RESET_HOLD_TICKS (5000 / BUTTON_POLL_MS)
#define SETUP_RESET_SHORT_CLICK_MAX_TICKS (500 / BUTTON_POLL_MS)
#define SETUP_RESET_COUNTDOWN_STEPS 12
#define SETUP_RESET_SOUND_ID "short-definite-fart"
#define BUTTON_INTERCOM_WAKE_WORD "push to intercom"

static void enter_setup_mode(const char *source, bool play_sound);

#if TATER_BOARD_VOICE_PE
#define VOICE_PE_VOLUME_STEP_PERCENT 5
#define VOICE_PE_ENCODER_POLL_MS 5
#define VOICE_PE_ENCODER_RESOLUTION_STEPS 2

typedef struct {
    bool stable_on;
    bool last_raw_on;
    int stable_count;
} voicepe_switch_t;

static bool voicepe_switch_update(voicepe_switch_t *sw, bool raw_on, bool *changed)
{
    if (changed) {
        *changed = false;
    }
    if (!sw) {
        return raw_on;
    }
    if (raw_on == sw->last_raw_on) {
        if (sw->stable_count < BUTTON_DEBOUNCE_TICKS) {
            sw->stable_count++;
        }
    } else {
        sw->stable_count = 0;
        sw->last_raw_on = raw_on;
    }
    if (sw->stable_count >= BUTTON_DEBOUNCE_TICKS && raw_on != sw->stable_on) {
        sw->stable_on = raw_on;
        if (changed) {
            *changed = true;
        }
    }
    return sw->stable_on;
}

static void voicepe_switch_init(voicepe_switch_t *sw, bool on)
{
    if (!sw) {
        return;
    }
    sw->stable_on = on;
    sw->last_raw_on = on;
    sw->stable_count = BUTTON_DEBOUNCE_TICKS;
}

static void voicepe_send_setting_feedback(const char *message)
{
    if (message && message[0]) {
        tater_protocol_send_log("info", message);
    }
    tater_protocol_send_status(tater_protocol_timer_is_active() ? "timer" : "idle");
}

static void voicepe_apply_volume_delta(int delta_percent)
{
    uint8_t volume = tater_live_settings_adjust_volume(delta_percent);
    const tater_live_settings_t *settings = tater_live_settings_get();
    tater_leds_show_volume(volume);
    char message[80] = {0};
    snprintf(message, sizeof(message), "Voice PE volume %u%%", volume);
    ESP_LOGI(
        TAG,
        "voice pe encoder delta=%d volume=%u muted=%d",
        delta_percent,
        volume,
        settings ? settings->muted : false
    );
    voicepe_send_setting_feedback(message);
}

static void voicepe_apply_mute_state(bool muted)
{
    bool current = tater_live_settings_set_muted(muted);
    tater_leds_show_mute(current);
    char message[80] = {0};
    snprintf(message, sizeof(message), "Voice PE microphones %s", current ? "muted" : "unmuted");
    ESP_LOGI(TAG, "voice pe microphones %s", current ? "muted" : "unmuted");
    if (current && tater_protocol_voice_active()) {
        tater_protocol_stop_voice(true);
    }
    voicepe_send_setting_feedback(message);
}

static uint8_t voicepe_encoder_state(void)
{
    uint8_t a = gpio_get_level(TATER_ENCODER_A) ? 1 : 0;
    uint8_t b = gpio_get_level(TATER_ENCODER_B) ? 1 : 0;
    return (uint8_t)(a | (b << 1));
}

static void voicepe_encoder_task(void *arg)
{
    (void)arg;
    static const int8_t transition_table[16] = {
         0,  1, -1,  0,
        -1,  0,  0,  1,
         1,  0,  0, -1,
         0, -1,  1,  0,
    };
    uint8_t last_state = voicepe_encoder_state();
    int detent_accum = 0;
    ESP_LOGI(TAG, "voice pe encoder ready state=0x%02x", last_state);
    while (true) {
        uint8_t state = voicepe_encoder_state();
        if (state != last_state) {
            int8_t movement = transition_table[((last_state & 0x03) << 2) | (state & 0x03)];
            if (movement != 0) {
                detent_accum += movement;
                while (detent_accum >= VOICE_PE_ENCODER_RESOLUTION_STEPS) {
                    voicepe_apply_volume_delta(VOICE_PE_VOLUME_STEP_PERCENT);
                    detent_accum -= VOICE_PE_ENCODER_RESOLUTION_STEPS;
                }
                while (detent_accum <= -VOICE_PE_ENCODER_RESOLUTION_STEPS) {
                    voicepe_apply_volume_delta(-VOICE_PE_VOLUME_STEP_PERCENT);
                    detent_accum += VOICE_PE_ENCODER_RESOLUTION_STEPS;
                }
            } else {
                detent_accum = 0;
            }
            last_state = state;
        }
        vTaskDelay(pdMS_TO_TICKS(VOICE_PE_ENCODER_POLL_MS));
    }
}

static void voicepe_poll_mute_switch(bool *initialized, voicepe_switch_t *mute_switch)
{
    bool muted = gpio_get_level(TATER_MUTE_SWITCH) != 0;
    if (initialized && !*initialized) {
        voicepe_switch_init(mute_switch, muted);
        tater_live_settings_set_muted(muted);
        ESP_LOGI(TAG, "voice pe mute switch ready muted=%d", muted);
        *initialized = true;
        return;
    }

    bool changed = false;
    bool stable_muted = voicepe_switch_update(mute_switch, muted, &changed);
    if (changed) {
        voicepe_apply_mute_state(stable_muted);
    }
}
#endif

#if TATER_BOARD_SAT1
#define SAT1_BUTTON_UP_MASK (1u << 0)
#define SAT1_BUTTON_DOWN_MASK (1u << 2)
#define SAT1_BUTTON_LEFT_MUTE_MASK (1u << 3)
#define SAT1_EXTRA_BUTTON_POLL_TICKS 1
#define SAT1_VOLUME_STEP_PERCENT 5

typedef struct {
    bool stable_pressed;
    bool last_raw_pressed;
    int stable_count;
} sat1_extra_button_t;

static void sat1_extra_button_init(sat1_extra_button_t *button, bool pressed)
{
    if (!button) {
        return;
    }
    button->stable_pressed = pressed;
    button->last_raw_pressed = pressed;
    button->stable_count = BUTTON_DEBOUNCE_TICKS;
}

static bool sat1_extra_button_update(sat1_extra_button_t *button, bool raw_pressed, bool *changed)
{
    if (!button) {
        if (changed) {
            *changed = false;
        }
        return false;
    }
    if (changed) {
        *changed = false;
    }
    if (raw_pressed == button->last_raw_pressed) {
        if (button->stable_count < BUTTON_DEBOUNCE_TICKS) {
            button->stable_count++;
        }
    } else {
        button->stable_count = 0;
        button->last_raw_pressed = raw_pressed;
    }
    if (button->stable_count >= BUTTON_DEBOUNCE_TICKS && raw_pressed != button->stable_pressed) {
        button->stable_pressed = raw_pressed;
        if (changed) {
            *changed = true;
        }
    }
    return button->stable_pressed;
}

static void sat1_send_setting_feedback(const char *message)
{
    if (message && message[0]) {
        tater_protocol_send_log("info", message);
    }
    tater_protocol_send_status(tater_protocol_timer_is_active() ? "timer" : "idle");
}

static void sat1_apply_volume_delta(int delta_percent)
{
    uint8_t volume = tater_live_settings_adjust_volume(delta_percent);
    const tater_live_settings_t *settings = tater_live_settings_get();
    tater_leds_show_volume(volume);
    char message[80] = {0};
    snprintf(message, sizeof(message), "Sat1 volume %u%%", volume);
    ESP_LOGI(
        TAG,
        "sat1 volume button delta=%d volume=%u muted=%d",
        delta_percent,
        volume,
        settings ? settings->muted : false
    );
    sat1_send_setting_feedback(message);
}

static void sat1_apply_mute_state(bool muted)
{
    bool current = tater_live_settings_set_muted(muted);
    tater_leds_show_mute(current);
    char message[80] = {0};
    snprintf(message, sizeof(message), "Sat1 microphones %s", current ? "muted" : "unmuted");
    ESP_LOGI(TAG, "sat1 microphones %s", current ? "muted" : "unmuted");
    if (current && tater_protocol_voice_active()) {
        tater_protocol_stop_voice(true);
    }
    sat1_send_setting_feedback(message);
}

static void sat1_poll_extra_buttons(
    bool *initialized,
    sat1_extra_button_t *up,
    sat1_extra_button_t *down,
    sat1_extra_button_t *left_mute
)
{
    uint8_t buttons = 0;
    static bool had_read_failure = false;
    if (tater_audio_sat1_read_buttons(&buttons) != ESP_OK) {
        static uint32_t read_failures = 0;
        read_failures++;
        if (read_failures == 1 || (read_failures % 100) == 0) {
            char message[96] = {0};
            snprintf(message, sizeof(message), "Sat1 button status read failed count=%lu", (unsigned long)read_failures);
            ESP_LOGW(TAG, "%s", message);
            tater_protocol_send_log("warn", message);
        }
        had_read_failure = true;
        return;
    }
    if (had_read_failure) {
        ESP_LOGI(TAG, "Sat1 button status read recovered");
        tater_protocol_send_log("info", "Sat1 button status read recovered");
        had_read_failure = false;
    }
    static bool have_last_buttons = false;
    static uint8_t last_buttons = 0;
    if (!have_last_buttons || buttons != last_buttons) {
        char raw_message[80] = {0};
        snprintf(raw_message, sizeof(raw_message), "Sat1 buttons raw=0x%02x", buttons);
        ESP_LOGI(TAG, "%s", raw_message);
        tater_protocol_send_log("info", raw_message);
        have_last_buttons = true;
        last_buttons = buttons;
    }

    bool up_pressed = (buttons & SAT1_BUTTON_UP_MASK) == 0;
    bool down_pressed = (buttons & SAT1_BUTTON_DOWN_MASK) == 0;
    bool muted = (buttons & SAT1_BUTTON_LEFT_MUTE_MASK) != 0;

    if (initialized && !*initialized) {
        sat1_extra_button_init(up, up_pressed);
        sat1_extra_button_init(down, down_pressed);
        sat1_extra_button_init(left_mute, muted);
        tater_live_settings_set_muted(muted);
        *initialized = true;
        ESP_LOGI(TAG, "sat1 extra buttons ready raw=0x%02x muted=%d", buttons, muted);
        return;
    }

    bool changed = false;
    if (sat1_extra_button_update(up, up_pressed, &changed) && changed) {
        sat1_apply_volume_delta(SAT1_VOLUME_STEP_PERCENT);
    }
    if (sat1_extra_button_update(down, down_pressed, &changed) && changed) {
        sat1_apply_volume_delta(-SAT1_VOLUME_STEP_PERCENT);
    }
    sat1_extra_button_update(left_mute, muted, &changed);
    if (changed) {
        sat1_apply_mute_state(left_mute ? left_mute->stable_pressed : muted);
    }
}
#endif

#if TATER_BOARD_RESPEAKER_XVF3800
#define XVF_SETUP_RESET_TOGGLE_COUNT 8
#define XVF_SETUP_RESET_TOGGLE_WINDOW_TICKS (8000 / BUTTON_POLL_MS)
#define XVF_SETUP_RESET_HOLD_TICKS (5000 / BUTTON_POLL_MS)

typedef struct {
    bool stable_on;
    bool last_raw_on;
    int stable_count;
} xvf_switch_t;

typedef struct {
    int toggle_count;
    int window_ticks;
    int hold_ticks;
    bool counting_down;
} xvf_setup_reset_t;

static void xvf_switch_init(xvf_switch_t *sw, bool on)
{
    if (!sw) {
        return;
    }
    sw->stable_on = on;
    sw->last_raw_on = on;
    sw->stable_count = BUTTON_DEBOUNCE_TICKS;
}

static bool xvf_switch_update(xvf_switch_t *sw, bool raw_on, bool *changed)
{
    if (changed) {
        *changed = false;
    }
    if (!sw) {
        return raw_on;
    }
    if (raw_on == sw->last_raw_on) {
        if (sw->stable_count < BUTTON_DEBOUNCE_TICKS) {
            sw->stable_count++;
        }
    } else {
        sw->stable_count = 0;
        sw->last_raw_on = raw_on;
    }
    if (sw->stable_count >= BUTTON_DEBOUNCE_TICKS && raw_on != sw->stable_on) {
        sw->stable_on = raw_on;
        if (changed) {
            *changed = true;
        }
    }
    return sw->stable_on;
}

static void xvf_send_setting_feedback(const char *message)
{
    if (message && message[0]) {
        tater_protocol_send_log("info", message);
    }
    tater_protocol_send_status(tater_protocol_timer_is_active() ? "timer" : "idle");
}

static void xvf_apply_mute_state(bool muted)
{
    bool current = tater_live_settings_set_muted(muted);
    tater_leds_show_mute(current);
    char message[96] = {0};
    snprintf(message, sizeof(message), "ReSpeaker microphones %s", current ? "muted" : "unmuted");
    ESP_LOGI(TAG, "respeaker microphones %s", current ? "muted" : "unmuted");
    if (current && tater_protocol_voice_active()) {
        tater_protocol_stop_voice(true);
    }
    xvf_send_setting_feedback(message);
}

static void xvf_setup_reset_clear(xvf_setup_reset_t *reset, bool clear_leds)
{
    if (!reset) {
        return;
    }
    reset->toggle_count = 0;
    reset->window_ticks = 0;
    reset->hold_ticks = 0;
    reset->counting_down = false;
    if (clear_leds) {
        tater_leds_clear_setup_reset_feedback();
    }
}

static void xvf_setup_reset_start_countdown(xvf_setup_reset_t *reset)
{
    if (!reset) {
        return;
    }
    reset->counting_down = true;
    reset->hold_ticks = 0;
    reset->window_ticks = 0;
    ESP_LOGW(TAG, "respeaker setup reset armed; leave mute on for 5 seconds");
    tater_protocol_send_log("warn", "ReSpeaker setup reset armed; leave mute on for 5 seconds.");
    tater_leds_show_setup_reset_countdown(SETUP_RESET_COUNTDOWN_STEPS, SETUP_RESET_COUNTDOWN_STEPS);
}

static void xvf_setup_reset_handle_toggle(xvf_setup_reset_t *reset, bool muted)
{
    if (!reset) {
        return;
    }
    if (tater_ota_is_running()) {
        xvf_setup_reset_clear(reset, true);
        ESP_LOGW(TAG, "respeaker setup reset gesture ignored during OTA");
        return;
    }
    if (reset->counting_down) {
        ESP_LOGI(TAG, "respeaker setup reset countdown cancelled by mute toggle");
        xvf_setup_reset_clear(reset, true);
        return;
    }
    if (reset->window_ticks <= 0) {
        reset->toggle_count = 0;
    }
    if (reset->toggle_count < XVF_SETUP_RESET_TOGGLE_COUNT) {
        reset->toggle_count++;
    }
    reset->window_ticks = XVF_SETUP_RESET_TOGGLE_WINDOW_TICKS;
    ESP_LOGI(TAG, "respeaker setup reset toggle %d/%d", reset->toggle_count, XVF_SETUP_RESET_TOGGLE_COUNT);

    if (reset->toggle_count >= XVF_SETUP_RESET_TOGGLE_COUNT && muted) {
        xvf_setup_reset_start_countdown(reset);
        return;
    }

    tater_leds_show_setup_reset_clicks((uint8_t)reset->toggle_count, XVF_SETUP_RESET_TOGGLE_COUNT);
    if (reset->toggle_count >= XVF_SETUP_RESET_TOGGLE_COUNT) {
        ESP_LOGW(TAG, "respeaker setup reset ready; toggle mute on to start countdown");
        tater_protocol_send_log("warn", "ReSpeaker setup reset ready; toggle mute on to start countdown.");
    }
}

static void xvf_setup_reset_tick(xvf_setup_reset_t *reset, bool muted)
{
    if (!reset) {
        return;
    }
    if (tater_ota_is_running()) {
        if (reset->toggle_count > 0 || reset->counting_down) {
            xvf_setup_reset_clear(reset, true);
        }
        return;
    }
    if (reset->counting_down) {
        if (!muted) {
            ESP_LOGI(TAG, "respeaker setup reset countdown cancelled because mute is off");
            xvf_setup_reset_clear(reset, true);
            return;
        }
        reset->hold_ticks++;
        uint8_t remaining_steps = (uint8_t)(
            SETUP_RESET_COUNTDOWN_STEPS
            - ((reset->hold_ticks * SETUP_RESET_COUNTDOWN_STEPS) / XVF_SETUP_RESET_HOLD_TICKS)
        );
        if (remaining_steps > SETUP_RESET_COUNTDOWN_STEPS) {
            remaining_steps = 0;
        }
        tater_leds_show_setup_reset_countdown(remaining_steps, SETUP_RESET_COUNTDOWN_STEPS);
        if (reset->hold_ticks >= XVF_SETUP_RESET_HOLD_TICKS) {
            enter_setup_mode("respeaker mute gesture", false);
        }
        return;
    }

    if (reset->window_ticks > 0) {
        reset->window_ticks--;
        if (reset->window_ticks <= 0) {
            ESP_LOGI(TAG, "respeaker setup reset gesture expired");
            xvf_setup_reset_clear(reset, true);
        }
    }
}

static void xvf_poll_mute_switch(bool *initialized, xvf_switch_t *mute_switch, xvf_setup_reset_t *reset)
{
    bool muted = false;
    static bool had_read_failure = false;
    if (tater_audio_xvf3800_read_mute(&muted) != ESP_OK) {
        static uint32_t read_failures = 0;
        read_failures++;
        if (read_failures == 1 || (read_failures % 100) == 0) {
            char message[96] = {0};
            snprintf(message, sizeof(message), "ReSpeaker mute status read failed count=%lu", (unsigned long)read_failures);
            ESP_LOGW(TAG, "%s", message);
            tater_protocol_send_log("warn", message);
        }
        had_read_failure = true;
        return;
    }
    if (had_read_failure) {
        ESP_LOGI(TAG, "ReSpeaker mute status read recovered");
        tater_protocol_send_log("info", "ReSpeaker mute status read recovered");
        had_read_failure = false;
    }

    if (initialized && !*initialized) {
        xvf_switch_init(mute_switch, muted);
        tater_live_settings_set_muted(muted);
        ESP_LOGI(TAG, "respeaker mute status ready muted=%d", muted);
        *initialized = true;
        return;
    }

    bool changed = false;
    bool stable_muted = xvf_switch_update(mute_switch, muted, &changed);
    if (changed) {
        xvf_apply_mute_state(stable_muted);
        xvf_setup_reset_handle_toggle(reset, stable_muted);
    }
    xvf_setup_reset_tick(reset, stable_muted);
}
#endif

esp_err_t tater_button_init(void)
{
#if TATER_HAS_CENTER_BUTTON
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TATER_CENTER_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
#else
    esp_err_t err = ESP_OK;
#endif
#if TATER_BOARD_VOICE_PE
    gpio_config_t encoder_cfg = {
        .pin_bit_mask = (1ULL << TATER_ENCODER_A) | (1ULL << TATER_ENCODER_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&encoder_cfg);
    if (err != ESP_OK) {
        return err;
    }
    gpio_config_t mute_cfg = {
        .pin_bit_mask = 1ULL << TATER_MUTE_SWITCH,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&mute_cfg);
    if (err != ESP_OK) {
        return err;
    }
#endif
#if TATER_BOARD_RESPEAKER_XVF3800
    err = tater_audio_xvf3800_control_init();
    if (err != ESP_OK) {
        return err;
    }
#endif
    return ESP_OK;
}

static void enter_setup_mode(const char *source, bool play_sound)
{
    ESP_LOGW(TAG, "setup reset requested by %s; clearing provisioning", source ? source : "button");
    tater_protocol_send_log("warn", "Setup reset requested; clearing provisioning and rebooting into setup mode.");
    if (tater_protocol_voice_active()) {
        tater_protocol_stop_voice(true);
    }
    if (tater_playback_is_playing()) {
        tater_playback_stop();
    }
    tater_leds_show_setup_reset_success();
    tater_protocol_send_status("provisioning");
    bool sound_started = false;
    if (play_sound) {
        const tater_wake_sound_asset_t *sound = tater_wake_sound_asset_lookup(SETUP_RESET_SOUND_ID);
        if (sound) {
            esp_err_t sound_err = tater_playback_play_wav_data_local(
                sound->data,
                (size_t)(sound->end - sound->data),
                SETUP_RESET_SOUND_ID
            );
            if (sound_err != ESP_OK) {
                ESP_LOGW(TAG, "setup reset sound failed: %s", esp_err_to_name(sound_err));
            }
        } else {
            ESP_LOGW(TAG, "setup reset sound asset not found: %s", SETUP_RESET_SOUND_ID);
        }
        for (int i = 0; i < 25; i++) {
            if (tater_playback_is_playing()) {
                sound_started = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    for (int i = 0; sound_started && i < 150 && tater_playback_is_playing(); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(tater_config_clear());
    tater_leds_clear_setup_reset_feedback();
    tater_leds_set_state(TATER_STATE_PROVISIONING);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void button_task(void *arg)
{
    bool stable_pressed = false;
    bool last_raw_pressed = false;
    int stable_count = 0;
    int press_ticks = 0;
    int setup_hold_ticks = 0;
    int setup_click_count = 0;
    int setup_click_window_ticks = 0;
    int setup_armed_ticks = 0;
    bool intercom_started_for_press = false;
    bool setup_hold_candidate = false;
    bool button_consumed_for_press = false;
#if TATER_BOARD_SAT1
    bool sat1_extra_buttons_initialized = false;
    int sat1_extra_button_poll_ticks = 0;
    sat1_extra_button_t sat1_button_up = {0};
    sat1_extra_button_t sat1_button_down = {0};
    sat1_extra_button_t sat1_button_left_mute = {0};
#endif
#if TATER_BOARD_VOICE_PE
    bool voicepe_mute_initialized = false;
    voicepe_switch_t voicepe_mute_switch = {0};
#endif
#if TATER_BOARD_RESPEAKER_XVF3800
    bool xvf_mute_initialized = false;
    xvf_switch_t xvf_mute_switch = {0};
    xvf_setup_reset_t xvf_setup_reset = {0};
#endif

    while (true) {
        bool raw_pressed = false;
#if TATER_HAS_CENTER_BUTTON
        raw_pressed = gpio_get_level(TATER_CENTER_BUTTON) == 0;
#endif
        if (raw_pressed == last_raw_pressed) {
            stable_count++;
        } else {
            stable_count = 0;
        }

        if (stable_count == BUTTON_DEBOUNCE_TICKS && raw_pressed != stable_pressed) {
            stable_pressed = raw_pressed;
            if (stable_pressed) {
                press_ticks = 0;
                setup_hold_ticks = 0;
                intercom_started_for_press = false;
                button_consumed_for_press = false;
                setup_hold_candidate = setup_click_count >= SETUP_RESET_CLICK_COUNT && setup_armed_ticks > 0;
                if (tater_protocol_timer_is_ringing()) {
                    ESP_LOGI(TAG, "center button press: stop timer alarm");
                    tater_protocol_timer_stop_from_device();
                    button_consumed_for_press = true;
                    setup_hold_candidate = false;
                    tater_leds_clear_setup_reset_feedback();
                } else if (tater_playback_is_playing()) {
                    ESP_LOGI(TAG, "center button press: stop playback");
                    tater_playback_stop();
                    button_consumed_for_press = true;
                    setup_hold_candidate = false;
                    tater_leds_clear_setup_reset_feedback();
                } else if (tater_protocol_voice_active()) {
                    ESP_LOGI(TAG, "center button press: cancel active voice");
                    tater_protocol_stop_voice(true);
                    button_consumed_for_press = true;
                    setup_hold_candidate = false;
                    tater_leds_clear_setup_reset_feedback();
                }
                if (setup_hold_candidate) {
                    ESP_LOGW(TAG, "setup reset armed; keep holding for 5 seconds");
                    tater_protocol_send_log("warn", "Setup reset armed; keep holding the button for 5 seconds.");
                    tater_leds_show_setup_reset_countdown(SETUP_RESET_COUNTDOWN_STEPS, SETUP_RESET_COUNTDOWN_STEPS);
                }
            } else {
                if (button_consumed_for_press) {
                    setup_click_count = 0;
                    setup_armed_ticks = 0;
                    setup_click_window_ticks = 0;
                } else if (setup_hold_candidate) {
                    ESP_LOGI(TAG, "setup reset hold cancelled");
                    tater_leds_clear_setup_reset_feedback();
                    setup_click_count = 0;
                    setup_armed_ticks = 0;
                    setup_click_window_ticks = 0;
                } else if (intercom_started_for_press) {
                    if (tater_protocol_voice_active()) {
                        ESP_LOGI(TAG, "center button release: intercom voice.stop");
                        tater_protocol_stop_voice(false);
                    }
                    setup_click_count = 0;
                    setup_armed_ticks = 0;
                    setup_click_window_ticks = 0;
                } else if (press_ticks > 0 && press_ticks <= SETUP_RESET_SHORT_CLICK_MAX_TICKS) {
                    if (setup_click_window_ticks <= 0) {
                        setup_click_count = 0;
                    }
                    setup_click_count++;
                    setup_click_window_ticks = SETUP_RESET_CLICK_WINDOW_TICKS;
                    ESP_LOGI(TAG, "setup reset click %d/%d", setup_click_count, SETUP_RESET_CLICK_COUNT);
                    tater_leds_show_setup_reset_clicks((uint8_t)setup_click_count, SETUP_RESET_CLICK_COUNT);
                    if (setup_click_count >= SETUP_RESET_CLICK_COUNT) {
                        setup_armed_ticks = SETUP_RESET_ARMED_TICKS;
                        ESP_LOGW(TAG, "setup reset armed; hold button for 5 seconds");
                        tater_protocol_send_log("warn", "Setup reset armed; press and hold the button for 5 seconds.");
                    }
                } else {
                    tater_leds_clear_setup_reset_feedback();
                    setup_click_count = 0;
                    setup_click_window_ticks = 0;
                    setup_armed_ticks = 0;
                }

                press_ticks = 0;
                setup_hold_ticks = 0;
                intercom_started_for_press = false;
                setup_hold_candidate = false;
                button_consumed_for_press = false;
            }
        }

        if (stable_pressed) {
            press_ticks++;
            if (setup_hold_candidate) {
                setup_hold_ticks++;
                uint8_t remaining_steps = (uint8_t)(
                    SETUP_RESET_COUNTDOWN_STEPS
                    - ((setup_hold_ticks * SETUP_RESET_COUNTDOWN_STEPS) / SETUP_RESET_HOLD_TICKS)
                );
                if (remaining_steps > SETUP_RESET_COUNTDOWN_STEPS) {
                    remaining_steps = 0;
                }
                tater_leds_show_setup_reset_countdown(remaining_steps, SETUP_RESET_COUNTDOWN_STEPS);
                if (setup_hold_ticks >= SETUP_RESET_HOLD_TICKS) {
                    enter_setup_mode("button gesture", true);
                }
            } else if (!button_consumed_for_press && !intercom_started_for_press && press_ticks >= INTERCOM_START_TICKS) {
                if (tater_ota_is_running()) {
                    ESP_LOGW(TAG, "button ignored during OTA");
                    button_consumed_for_press = true;
                } else {
                    ESP_LOGI(TAG, "center button hold: intercom voice.start");
                    tater_leds_clear_setup_reset_feedback();
                    tater_protocol_start_voice(BUTTON_INTERCOM_WAKE_WORD, "center_button_hold");
                    intercom_started_for_press = true;
                    setup_click_count = 0;
                    setup_click_window_ticks = 0;
                    setup_armed_ticks = 0;
                }
            }
        } else {
            if (setup_click_window_ticks > 0) {
                setup_click_window_ticks--;
                if (setup_click_window_ticks <= 0 && setup_armed_ticks <= 0) {
                    ESP_LOGI(TAG, "setup reset click sequence expired");
                    tater_leds_clear_setup_reset_feedback();
                    setup_click_count = 0;
                }
            }
            if (setup_armed_ticks > 0) {
                setup_armed_ticks--;
                if (setup_armed_ticks <= 0) {
                    ESP_LOGI(TAG, "setup reset arm expired");
                    tater_leds_clear_setup_reset_feedback();
                    setup_click_count = 0;
                    setup_click_window_ticks = 0;
                }
            }
        }

        last_raw_pressed = raw_pressed;
#if TATER_BOARD_SAT1
        sat1_extra_button_poll_ticks++;
        if (sat1_extra_button_poll_ticks >= SAT1_EXTRA_BUTTON_POLL_TICKS) {
            sat1_extra_button_poll_ticks = 0;
            sat1_poll_extra_buttons(
                &sat1_extra_buttons_initialized,
                &sat1_button_up,
                &sat1_button_down,
                &sat1_button_left_mute
            );
        }
#endif
#if TATER_BOARD_VOICE_PE
        voicepe_poll_mute_switch(&voicepe_mute_initialized, &voicepe_mute_switch);
#endif
#if TATER_BOARD_RESPEAKER_XVF3800
        xvf_poll_mute_switch(&xvf_mute_initialized, &xvf_mute_switch, &xvf_setup_reset);
#endif
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

void tater_button_start_task(void)
{
    xTaskCreatePinnedToCore(button_task, "tater_button", 4096, NULL, 5, NULL, 0);
#if TATER_BOARD_VOICE_PE
    xTaskCreatePinnedToCore(voicepe_encoder_task, "tater_encoder", 3072, NULL, 5, NULL, 0);
#endif
}
