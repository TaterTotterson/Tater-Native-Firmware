#include "audio_i2s.h"
#include "board.h"
#include "button.h"
#include "cache_storage.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "leds.h"
#include "ota_update.h"
#include "playback.h"
#include "provisioning.h"
#include <stdbool.h>
#include "tater_config.h"
#include "tater_protocol.h"
#include "wake_engine.h"
#include "wifi_station.h"

#include <inttypes.h>

static const char *TAG = "tater_app";
static tater_config_t s_config;

static void set_speaker_amp(bool enabled)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TATER_SPK_AMP_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(TATER_SPK_AMP_EN, enabled ? 1 : 0);
}

static void on_tater_state(tater_state_t state, const char *detail)
{
    ESP_LOGI(TAG, "state=%d detail=%s", (int)state, detail ? detail : "");
    tater_leds_set_state(state);
}

static void on_tater_play_url(const char *url, tater_state_t visual_state)
{
    ESP_LOGI(TAG, "play.url received: %s", url ? url : "");
    tater_leds_set_state(visual_state);
    esp_err_t err = tater_playback_play_url(url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "playback start failed: %s", esp_err_to_name(err));
        tater_protocol_send_log("error", "playback start failed");
    }
}

static void on_tater_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent)
{
    ESP_LOGI(
        TAG,
        "play.tone received: frequency=%" PRIu32 " duration_ms=%" PRIu32 " volume_percent=%u",
        frequency_hz,
        duration_ms,
        volume_percent
    );
    tater_leds_set_state(TATER_STATE_SPEAKING);
    esp_err_t err = tater_playback_play_tone(frequency_hz, duration_ms, volume_percent);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tone start failed: %s", esp_err_to_name(err));
        tater_protocol_send_log("error", "tone start failed");
    }
}

static void on_tater_ota_url(const char *url)
{
    ESP_LOGW(TAG, "ota.url received: %s", url ? url : "");
    tater_playback_stop();
    tater_leds_set_state(TATER_STATE_OTA);
    esp_err_t err = tater_ota_start_url(url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA start failed: %s", esp_err_to_name(err));
        tater_protocol_send_ota_status("error", 0, esp_err_to_name(err));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK_WITHOUT_ABORT(tater_cache_init());

    ESP_ERROR_CHECK(tater_config_load(&s_config));
    ESP_ERROR_CHECK(tater_leds_init());
    ESP_ERROR_CHECK(tater_button_init());

    if (!tater_config_has_wifi(&s_config)) {
        ESP_LOGW(TAG, "no Wi-Fi config; starting provisioning");
        tater_leds_set_state(TATER_STATE_PROVISIONING);
        ESP_ERROR_CHECK(tater_provisioning_start(&s_config));
        int tick = 0;
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (++tick >= 10) {
                ESP_LOGI(
                    TAG,
                    "provisioning active; connect to %s then open http://192.168.4.1",
                    tater_provisioning_ssid()
                );
                tick = 0;
            }
        }
    }

    set_speaker_amp(true);
    ESP_ERROR_CHECK(tater_audio_i2s_init());
    ESP_ERROR_CHECK(tater_playback_init());

    err = tater_wifi_connect(&s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connection failed; returning to setup mode on next boot");
        tater_leds_set_state(TATER_STATE_ERROR);
        ESP_ERROR_CHECK_WITHOUT_ABORT(tater_config_forget_wifi());
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    tater_protocol_init(&s_config, on_tater_state, on_tater_play_url, on_tater_play_tone, on_tater_ota_url);
    tater_protocol_start();
    ESP_ERROR_CHECK_WITHOUT_ABORT(tater_wake_engine_init());

    tater_audio_i2s_start_task();
    tater_button_start_task();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(
            TAG,
            "heartbeat connected=%d voice_active=%d free_heap=%lu",
            tater_protocol_is_connected(),
            tater_protocol_voice_active(),
            (unsigned long)esp_get_free_heap_size()
        );
        const char *status_state = tater_protocol_timer_is_active()
            ? "timer"
            : (tater_protocol_voice_active() ? "listening" : "idle");
        tater_protocol_send_status(status_state);
    }
}
