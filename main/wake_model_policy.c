#include "wake_model_policy.h"

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
