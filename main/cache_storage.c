#include "cache_storage.h"

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "tater_cache";
static const char *CACHE_BASE_PATH = "/spiffs";
static const char *CACHE_PARTITION_LABEL = "storage";

static bool s_ready;

esp_err_t tater_cache_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = CACHE_BASE_PATH,
        .partition_label = CACHE_PARTITION_LABEL,
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info(conf.partition_label, &total, &used);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS info failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "cache mounted total=%u used=%u", (unsigned)total, (unsigned)used);
    }
    s_ready = true;
    return ESP_OK;
}

esp_err_t tater_cache_format(void)
{
    if (s_ready) {
        esp_err_t unregister_err = esp_vfs_spiffs_unregister(CACHE_PARTITION_LABEL);
        if (unregister_err != ESP_OK) {
            ESP_LOGW(TAG, "SPIFFS unregister failed before format: %s", esp_err_to_name(unregister_err));
        }
        s_ready = false;
    }

    esp_err_t err = esp_spiffs_format(CACHE_PARTITION_LABEL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS format failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "cache formatted");
    return tater_cache_init();
}

bool tater_cache_ready(void)
{
    return s_ready;
}

const char *tater_cache_base_path(void)
{
    return CACHE_BASE_PATH;
}
