#include "display_clock.h"

#include <stdio.h>

bool tater_display_format_local_clock(
    uint32_t seconds_since_midnight,
    char *time_text,
    size_t time_text_len,
    char *ampm_text,
    size_t ampm_text_len
)
{
    if (!time_text || time_text_len == 0 || !ampm_text || ampm_text_len == 0) {
        return false;
    }

    uint32_t day_seconds = seconds_since_midnight % (24U * 60U * 60U);
    uint32_t hour_24 = day_seconds / (60U * 60U);
    uint32_t minute = (day_seconds / 60U) % 60U;
    uint32_t hour_12 = hour_24 % 12U;
    if (hour_12 == 0) {
        hour_12 = 12;
    }

    int time_written = snprintf(time_text, time_text_len, "%lu:%02lu", (unsigned long)hour_12, (unsigned long)minute);
    int ampm_written = snprintf(ampm_text, ampm_text_len, "%s", hour_24 >= 12U ? "PM" : "AM");
    return time_written > 0 && (size_t)time_written < time_text_len
        && ampm_written > 0 && (size_t)ampm_written < ampm_text_len;
}
