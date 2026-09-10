#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../display_clock.h"

static void assert_clock(uint32_t seconds, const char *expected_time, const char *expected_ampm)
{
    char time_text[12] = {0};
    char ampm_text[8] = {0};
    assert(tater_display_format_local_clock(
        seconds,
        time_text,
        sizeof(time_text),
        ampm_text,
        sizeof(ampm_text)
    ));
    assert(strcmp(time_text, expected_time) == 0);
    assert(strcmp(ampm_text, expected_ampm) == 0);
}

int main(void)
{
    assert_clock(0, "12:00", "AM");
    assert_clock((8U * 60U * 60U) + (37U * 60U), "8:37", "AM");
    assert_clock(12U * 60U * 60U, "12:00", "PM");
    assert_clock((23U * 60U * 60U) + (59U * 60U), "11:59", "PM");
    assert_clock(24U * 60U * 60U, "12:00", "AM");

    char time_text[2] = {0};
    char ampm_text[8] = {0};
    assert(!tater_display_format_local_clock(0, time_text, sizeof(time_text), ampm_text, sizeof(ampm_text)));

    puts("Display clock host tests passed.");
    return 0;
}
