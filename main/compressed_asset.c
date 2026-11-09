#include "compressed_asset.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"

static const char *TAG = "tater_asset";

esp_err_t tater_compressed_asset_expand(
    const uint8_t *compressed,
    size_t compressed_size,
    size_t expected_size,
    uint8_t **out
)
{
    if (!compressed || compressed_size == 0 || expected_size == 0 || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    uint8_t *expanded = (uint8_t *)heap_caps_malloc(
        expected_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!expanded) {
        ESP_LOGE(TAG, "compressed asset PSRAM allocation failed bytes=%u", (unsigned)expected_size);
        return ESP_ERR_NO_MEM;
    }

    size_t written = tinfl_decompress_mem_to_mem(
        expanded,
        expected_size,
        compressed,
        compressed_size,
        TINFL_FLAG_PARSE_ZLIB_HEADER
    );
    if (written != expected_size) {
        ESP_LOGE(
            TAG,
            "compressed asset expansion failed compressed=%u expected=%u written=%u",
            (unsigned)compressed_size,
            (unsigned)expected_size,
            (unsigned)written
        );
        heap_caps_free(expanded);
        return ESP_ERR_INVALID_CRC;
    }

    *out = expanded;
    ESP_LOGI(
        TAG,
        "compressed asset expanded compressed=%u raw=%u",
        (unsigned)compressed_size,
        (unsigned)expected_size
    );
    return ESP_OK;
}
