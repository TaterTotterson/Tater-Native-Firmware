#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool tater_display_format_local_clock(
    uint32_t seconds_since_midnight,
    char *time_text,
    size_t time_text_len,
    char *ampm_text,
    size_t ampm_text_len
);

#ifdef __cplusplus
}
#endif
