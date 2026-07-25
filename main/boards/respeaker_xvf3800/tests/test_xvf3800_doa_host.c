#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../xvf3800_doa.h"

static void expect_direction(uint16_t degrees, uint8_t expected_led)
{
    uint8_t response[5] = {
        0,
        (uint8_t)(degrees & 0xff),
        (uint8_t)(degrees >> 8),
        1,
        0,
    };
    xvf3800_doa_value_t value = {0};
    if (!xvf3800_doa_decode(response, sizeof(response), 12, 5, &value) ||
        !value.speech_detected ||
        value.angle_index != expected_led) {
        fprintf(
            stderr,
            "direction failed: degrees=%u speech=%u led=%u expected=%u\n",
            degrees,
            value.speech_detected,
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

    /*
     * The XVF3800 may keep changing its reported angle after speech ends.
     * Every such frame must remain invalid when its hardware speech detector
     * is clear, regardless of the angle bytes.
     */
    for (uint16_t degrees = 0; degrees < 360; degrees += 17) {
        uint8_t response[5] = {
            0,
            (uint8_t)(degrees & 0xff),
            (uint8_t)(degrees >> 8),
            0,
            0,
        };
        xvf3800_doa_value_t value = {0};
        if (!xvf3800_doa_decode(response, sizeof(response), 12, 5, &value) ||
            value.speech_detected) {
            fprintf(stderr, "silence gate failed: degrees=%u\n", degrees);
            return 1;
        }
    }

    uint8_t invalid_angle[5] = {0, 0x68, 0x01, 1, 0};
    xvf3800_doa_value_t value = {0};
    if (xvf3800_doa_decode(
            invalid_angle,
            sizeof(invalid_angle),
            12,
            5,
            &value)) {
        fputs("invalid speech angle was accepted\n", stderr);
        return 1;
    }

    puts("XVF3800 DoA host tests passed.");
    return 0;
}
