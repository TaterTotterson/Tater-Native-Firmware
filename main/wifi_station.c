#include "wifi_station.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "network_recovery.h"

static const char *TAG = "tater_wifi";
static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;
static TaskHandle_t s_wifi_reconnect_task;
static portMUX_TYPE s_wifi_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_wifi_has_ip;
static uint32_t s_retry_count;

static bool wifi_has_ip(void)
{
    portENTER_CRITICAL(&s_wifi_state_lock);
    bool connected = s_wifi_has_ip;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    return connected;
}

static uint32_t next_retry_attempt(void)
{
    portENTER_CRITICAL(&s_wifi_state_lock);
    uint32_t attempt = ++s_retry_count;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    return attempt;
}

static void schedule_wifi_reconnect(void)
{
    if (s_wifi_reconnect_task) {
        xTaskNotifyGive(s_wifi_reconnect_task);
    }
}

static void wifi_reconnect_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (wifi_has_ip()) {
            continue;
        }

        uint32_t attempt = next_retry_attempt();
        uint32_t delay_ms = tater_wifi_retry_delay_ms(attempt, esp_random());
        ESP_LOGW(
            TAG,
            "wifi reconnect scheduled attempt=%lu delay_ms=%lu",
            (unsigned long)attempt,
            (unsigned long)delay_ms
        );
        if (delay_ms > 0U) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        if (wifi_has_ip()) {
            continue;
        }

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi reconnect attempt=%lu failed to start: %s", (unsigned long)attempt, esp_err_to_name(err));
            schedule_wifi_reconnect();
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        schedule_wifi_reconnect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_has_ip = false;
        portEXIT_CRITICAL(&s_wifi_state_lock);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "wifi disconnected reason=%d rssi=%d", event ? event->reason : -1, event ? event->rssi : 0);
        schedule_wifi_reconnect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
        ESP_LOGI(TAG, "wifi power save disabled after connect");
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_has_ip = true;
        s_retry_count = 0;
        portEXIT_CRITICAL(&s_wifi_state_lock);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t tater_wifi_connect(const tater_config_t *config)
{
    if (!config || !tater_config_has_wifi(config)) {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty; start provisioning");
        return ESP_ERR_INVALID_ARG;
    }

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(wifi_reconnect_task, "tater_wifi_retry", 3072, NULL, 5, &s_wifi_reconnect_task) != pdPASS) {
        s_wifi_reconnect_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, config->wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, config->wifi_password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "wifi power save disabled for native voice streaming");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(30000)
    );
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to %s", config->wifi_ssid);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "initial connection to %s timed out; background recovery will continue", config->wifi_ssid);
    return ESP_ERR_TIMEOUT;
}
