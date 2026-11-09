#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TATER_DOWNLOAD_INTERNAL_FALLBACK_MAX_BYTES (32U * 1024U)

#ifdef __cplusplus
extern "C" {
#endif

bool tater_wake_model_config_changed(
    const char *current_wake_word,
    const char *current_wake_word_url,
    const char *current_revision,
    const char *next_wake_word,
    const char *next_wake_word_url,
    const char *next_revision
);

bool tater_background_download_allowed(bool ota_running);
size_t tater_download_allocation_size(
    int64_t content_length,
    size_t max_bytes,
    bool null_terminate
);
bool tater_download_internal_fallback_allowed(size_t allocation_size);

#ifdef __cplusplus
}
#endif
