#include "button.h"

#include <stdbool.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "ota_update.h"
#include "playback.h"
#include "tater_config.h"
#include "tater_protocol.h"
#include "wake_sound_assets.h"

static const char *TAG = "tater_button";

#define BUTTON_POLL_MS 20
#define BUTTON_DEBOUNCE_TICKS 3
#define VOICE_START_TICKS 8
#define SETUP_RESET_CLICK_COUNT 5
#define SETUP_RESET_CLICK_WINDOW_TICKS (3000 / BUTTON_POLL_MS)
#define SETUP_RESET_ARMED_TICKS (5000 / BUTTON_POLL_MS)
#define SETUP_RESET_HOLD_TICKS (5000 / BUTTON_POLL_MS)
#define SETUP_RESET_SHORT_CLICK_MAX_TICKS (500 / BUTTON_POLL_MS)
#define SETUP_RESET_COUNTDOWN_STEPS 12
#define SETUP_RESET_SOUND_ID "short-definite-fart"

esp_err_t tater_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TATER_CENTER_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static void enter_setup_mode(const char *source)
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
    bool sound_started = false;
    for (int i = 0; i < 25; i++) {
        if (tater_playback_is_playing()) {
            sound_started = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
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
    bool voice_started_for_press = false;
    bool setup_hold_candidate = false;
    bool timer_stopped_for_press = false;

    while (true) {
        bool raw_pressed = gpio_get_level(TATER_CENTER_BUTTON) == 0;
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
                voice_started_for_press = false;
                timer_stopped_for_press = false;
                setup_hold_candidate = setup_click_count >= SETUP_RESET_CLICK_COUNT && setup_armed_ticks > 0;
                if (tater_protocol_timer_is_ringing()) {
                    ESP_LOGI(TAG, "center button press: stop timer alarm");
                    tater_protocol_timer_stop_from_device();
                    timer_stopped_for_press = true;
                    setup_hold_candidate = false;
                    tater_leds_clear_setup_reset_feedback();
                }
                if (setup_hold_candidate) {
                    ESP_LOGW(TAG, "setup reset armed; keep holding for 5 seconds");
                    tater_protocol_send_log("warn", "Setup reset armed; keep holding the button for 5 seconds.");
                    tater_leds_show_setup_reset_countdown(SETUP_RESET_COUNTDOWN_STEPS, SETUP_RESET_COUNTDOWN_STEPS);
                }
            } else {
                if (timer_stopped_for_press) {
                    setup_click_count = 0;
                    setup_armed_ticks = 0;
                    setup_click_window_ticks = 0;
                } else if (setup_hold_candidate) {
                    ESP_LOGI(TAG, "setup reset hold cancelled");
                    tater_leds_clear_setup_reset_feedback();
                    setup_click_count = 0;
                    setup_armed_ticks = 0;
                    setup_click_window_ticks = 0;
                } else if (!voice_started_for_press && press_ticks > 0 && press_ticks <= SETUP_RESET_SHORT_CLICK_MAX_TICKS) {
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

                if (voice_started_for_press && tater_protocol_voice_active()) {
                    ESP_LOGI(TAG, "center button release: voice.stop");
                    tater_protocol_stop_voice(false);
                }
                press_ticks = 0;
                setup_hold_ticks = 0;
                voice_started_for_press = false;
                setup_hold_candidate = false;
                timer_stopped_for_press = false;
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
                    enter_setup_mode("button gesture");
                }
            } else if (!timer_stopped_for_press && !voice_started_for_press && press_ticks >= VOICE_START_TICKS) {
                if (tater_ota_is_running()) {
                    ESP_LOGW(TAG, "button ignored during OTA");
                } else {
                    if (tater_playback_is_playing()) {
                        tater_playback_stop();
                    }
                    ESP_LOGI(TAG, "center button hold: voice.start");
                    tater_leds_clear_setup_reset_feedback();
                    tater_protocol_start_voice("", "center_button");
                    voice_started_for_press = true;
                    setup_click_count = 0;
                    setup_click_window_ticks = 0;
                    setup_armed_ticks = 0;
                }
            }
        } else {
            if (setup_click_window_ticks > 0) {
                setup_click_window_ticks--;
                if (setup_click_window_ticks <= 0 && setup_armed_ticks <= 0) {
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
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

void tater_button_start_task(void)
{
    xTaskCreatePinnedToCore(button_task, "tater_button", 4096, NULL, 5, NULL, 0);
}
