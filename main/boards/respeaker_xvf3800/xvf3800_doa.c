#include "xvf3800_doa.h"

#include <math.h>
#include <string.h>

#define XVF3800_CONTROL_DONE (0)
#define XVF3800_AZIMUTH_RESPONSE_LEN (17)
#define XVF3800_AUTO_SELECT_BEAM_OFFSET (13)
#define XVF3800_PI (3.14159265358979323846f)
#define XVF3800_TAU (2.0f * XVF3800_PI)

bool xvf3800_doa_decode(const uint8_t *response,
                        size_t response_len,
                        uint8_t led_count,
                        int led_offset,
                        xvf3800_doa_value_t *out)
{
    if (!response ||
        response_len < XVF3800_AZIMUTH_RESPONSE_LEN ||
        led_count == 0 ||
        !out ||
        response[0] != XVF3800_CONTROL_DONE) {
        return false;
    }

    float radians = 0.0f;
    memcpy(
        &radians,
        &response[XVF3800_AUTO_SELECT_BEAM_OFFSET],
        sizeof(radians));
    if (!isfinite(radians)) {
        return false;
    }
    while (radians < 0.0f) {
        radians += XVF3800_TAU;
    }
    while (radians >= XVF3800_TAU) {
        radians -= XVF3800_TAU;
    }

    memset(out, 0, sizeof(*out));
    out->radians = radians;
    out->degrees = (uint16_t)(
        (radians * 180.0f / XVF3800_PI) + 0.5f);
    if (out->degrees >= 360) {
        out->degrees = 0;
    }
    int raw_index =
        (int)((radians * (float)led_count / XVF3800_TAU) + 0.5f);
    int angle_index = (raw_index + led_offset) % led_count;
    if (angle_index < 0) {
        angle_index += led_count;
    }
    out->angle_index = (uint8_t)angle_index;
    return true;
}
