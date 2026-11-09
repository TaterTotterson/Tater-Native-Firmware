#include "wake_model_policy.h"

#include <stdint.h>
#include <string.h>

static const char *text_or_empty(const char *value)
{
    return value ? value : "";
}

bool tater_wake_model_config_changed(
    const char *current_wake_word,
    const char *current_wake_word_url,
    const char *current_revision,
    const char *next_wake_word,
    const char *next_wake_word_url,
    const char *next_revision
)
{
    return strcmp(text_or_empty(current_wake_word), text_or_empty(next_wake_word)) != 0
        || strcmp(text_or_empty(current_wake_word_url), text_or_empty(next_wake_word_url)) != 0
        || strcmp(text_or_empty(current_revision), text_or_empty(next_revision)) != 0;
}

bool tater_background_download_allowed(bool ota_running)
{
    return !ota_running;
}

size_t tater_download_allocation_size(
    int64_t content_length,
    size_t max_bytes,
    bool null_terminate
)
{
    if (max_bytes == 0 || content_length > (int64_t)max_bytes) {
        return 0;
    }

    size_t payload_size = content_length > 0
        ? (size_t)content_length
        : max_bytes;
    if (null_terminate && payload_size == SIZE_MAX) {
        return 0;
    }
    return payload_size + (null_terminate ? 1U : 0U);
}

bool tater_download_internal_fallback_allowed(size_t allocation_size)
{
    return allocation_size > 0
        && allocation_size <= TATER_DOWNLOAD_INTERNAL_FALLBACK_MAX_BYTES;
}
