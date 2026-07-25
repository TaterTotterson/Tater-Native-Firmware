#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../xvf3800_doa.h"

#define TEST_PI (3.14159265358979323846f)

static void expect_direction(float degrees, uint8_t expected_led)
{
    uint8_t response[17] = {0};
    float radians = degrees * TEST_PI / 180.0f;
    memcpy(&response[13], &radians, sizeof(radians));
    xvf3800_doa_value_t value = {0};
    if (!xvf3800_doa_decode(response, sizeof(response), 12, 5, &value) ||
        value.angle_index != expected_led) {
        fprintf(
            stderr,
            "direction failed: degrees=%.1f decoded=%u led=%u expected=%u\n",
            degrees,
            value.degrees,
            value.angle_index,
            expected_led);
        exit(1);
    }
}

int main(void)
{
    expect_direction(0, 5);
    expect_direction(30, 6);
    expect_direction(180, 11);
    expect_direction(330, 4);
    expect_direction(359, 5);
    expect_direction(-30, 4);
    expect_direction(390, 6);

    uint8_t wait_response[17] = {1};
    xvf3800_doa_value_t value = {0};
    if (xvf3800_doa_decode(
            wait_response,
            sizeof(wait_response),
            12,
            5,
            &value)) {
        fputs("busy azimuth response was accepted\n", stderr);
        return 1;
    }

    uint8_t invalid_angle[17] = {0};
    float invalid_radians = NAN;
    memcpy(&invalid_angle[13], &invalid_radians, sizeof(invalid_radians));
    if (xvf3800_doa_decode(
            invalid_angle,
            sizeof(invalid_angle),
            12,
            5,
            &value)) {
        fputs("non-finite azimuth was accepted\n", stderr);
        return 1;
    }

    puts("XVF3800 DoA host tests passed.");
    return 0;
}
