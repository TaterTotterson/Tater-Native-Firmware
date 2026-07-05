#include "ota_update.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tater_protocol.h"

static const char *TAG = "tater_ota";
static volatile bool s_running;

static void ota_task(void *arg)
{
    char *url = (char *)arg;
    esp_ota_handle_t ota = 0;
    const esp_partition_t *partition = NULL;
    uint8_t *buf = NULL;
    int last_progress = -1;
    esp_err_t err = ESP_OK;

    s_running = true;
    tater_protocol_send_ota_status("starting", 0, "OTA starting");
    tater_protocol_send_log("info", "OTA starting");

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        goto done;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if (content_len > 0 && content_len > partition->size) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    ESP_LOGI(TAG, "writing %s to partition %s at 0x%lx", url, partition->label, (unsigned long)partition->address);
    err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) {
        goto done;
    }

    buf = malloc(4096);
    if (!buf) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    int64_t written = 0;
    while (true) {
        int got = esp_http_client_read(client, (char *)buf, 4096);
        if (got < 0) {
            err = ESP_FAIL;
            goto done;
        }
        if (got == 0) {
            break;
        }
        err = esp_ota_write(ota, buf, got);
        if (err != ESP_OK) {
            goto done;
        }
        written += got;
        if (content_len > 0) {
            int progress = (int)((written * 100) / content_len);
            if (progress != last_progress && (progress == 100 || progress % 5 == 0)) {
                last_progress = progress;
                tater_protocol_send_ota_status("writing", progress, "OTA writing");
            }
        }
    }

    err = esp_ota_end(ota);
    ota = 0;
    if (err != ESP_OK) {
        goto done;
    }
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        goto done;
    }

    tater_protocol_send_ota_status("rebooting", 100, "OTA complete; rebooting");
    tater_protocol_send_log("info", "OTA complete; rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

done:
    if (ota) {
        esp_ota_abort(ota);
    }
    if (buf) {
        free(buf);
    }
    if (client) {
        esp_http_client_cleanup(client);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        tater_protocol_send_ota_status("error", last_progress > 0 ? last_progress : 0, esp_err_to_name(err));
        tater_protocol_send_log("error", "OTA failed");
    }
    free(url);
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t tater_ota_start_url(const char *url)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!url || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = strdup(url);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(ota_task, "tater_ota", 8192, copy, 7, NULL, 1);
    if (ok != pdPASS) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool tater_ota_is_running(void)
{
    return s_running;
}
