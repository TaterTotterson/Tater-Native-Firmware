#include "ota_update.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tater_protocol.h"

static const char *TAG = "tater_ota";
static volatile bool s_running;

#define OTA_HTTP_BUFFER_SIZE 1024
#define OTA_READ_BUFFER_SIZE 1024
#define OTA_TASK_STACK_SIZE 6144

static void ota_send_logf(const char *level, const char *fmt, ...)
{
    char msg[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    tater_protocol_send_log(level, msg);
}

static void ota_log_heap(const char *stage)
{
    ota_send_logf(
        "debug",
        "OTA %s heap free=%u internal=%u largest_internal=%u",
        stage ? stage : "stage",
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
    );
}

static void ota_task(void *arg)
{
    char *url = (char *)arg;
    esp_ota_handle_t ota = 0;
    const esp_partition_t *partition = NULL;
    uint8_t *buf = NULL;
    int last_progress = -1;
    const char *stage = "starting";
    esp_err_t err = ESP_OK;

    s_running = true;
    tater_protocol_send_ota_status("starting", 0, "OTA starting");
    tater_protocol_send_log("info", "OTA download starting");
    ota_log_heap("starting");

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
    };
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    stage = "http client init";
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    stage = "http open";
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        goto done;
    }

    stage = "http headers";
    int64_t content_len = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        ota_send_logf("error", "OTA HTTP status %d", status_code);
        err = ESP_FAIL;
        stage = "http status";
        goto done;
    }

    stage = "partition select";
    partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if (content_len > 0 && content_len > partition->size) {
        ota_send_logf(
            "error",
            "OTA image too large: %lld > %u",
            (long long)content_len,
            (unsigned)partition->size
        );
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    ESP_LOGI(
        TAG,
        "writing %s to partition %s at 0x%lx size=%lld",
        url,
        partition->label,
        (unsigned long)partition->address,
        (long long)content_len
    );
    ota_send_logf("info", "OTA writing to %s", partition->label);
    stage = "ota begin";
    err = esp_ota_begin(partition, content_len > 0 ? content_len : OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) {
        goto done;
    }

    stage = "read buffer alloc";
    buf = heap_caps_malloc(OTA_READ_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    int64_t written = 0;
    while (true) {
        stage = "http read";
        int got = esp_http_client_read(client, (char *)buf, OTA_READ_BUFFER_SIZE);
        if (got < 0) {
            err = ESP_FAIL;
            goto done;
        }
        if (got == 0) {
            break;
        }
        stage = "ota write";
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

    stage = "ota end";
    err = esp_ota_end(ota);
    ota = 0;
    if (err != ESP_OK) {
        goto done;
    }
    stage = "set boot partition";
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
        ota_send_logf(
            "error",
            "OTA failed during %s: %s heap=%u internal=%u largest_internal=%u",
            stage,
            esp_err_to_name(err),
            (unsigned)esp_get_free_heap_size(),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
        );
        char status_msg[128];
        snprintf(status_msg, sizeof(status_msg), "OTA failed during %s: %s", stage, esp_err_to_name(err));
        tater_protocol_send_ota_status("error", last_progress > 0 ? last_progress : 0, status_msg);
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
    s_running = true;
    char *copy = strdup(url);
    if (!copy) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(ota_task, "tater_ota", OTA_TASK_STACK_SIZE, copy, 7, NULL, 1);
    if (ok != pdPASS) {
        free(copy);
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool tater_ota_is_running(void)
{
    return s_running;
}
