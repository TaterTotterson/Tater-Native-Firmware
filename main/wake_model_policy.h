#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
