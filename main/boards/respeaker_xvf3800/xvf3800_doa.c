#include "xvf3800_doa.h"

#include <string.h>

#define XVF3800_CONTROL_DONE (0)
#define XVF3800_DOA_RESPONSE_LEN (5)

static uint16_t read_u16_le(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

bool xvf3800_doa_decode(const uint8_t *response,
                        size_t response_len,
                        uint8_t led_count,
                        int led_offset,
                        xvf3800_doa_value_t *out)
{
    if (!response ||
        response_len < XVF3800_DOA_RESPONSE_LEN ||
        led_count == 0 ||
        !out ||
        response[0] != XVF3800_CONTROL_DONE) {
        return false;
    }

    uint16_t degrees = read_u16_le(&response[1]);
    uint16_t speech_detected = read_u16_le(&response[3]);
    if (speech_detected != 0 && degrees >= 360) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->degrees = degrees;
    out->speech_detected = speech_detected != 0;
    if (!out->speech_detected) {
        return true;
    }

    int raw_index =
        (((int)degrees * (int)led_count) + 180) / 360;
    int angle_index = (raw_index + led_offset) % led_count;
    if (angle_index < 0) {
        angle_index += led_count;
    }
    out->angle_index = (uint8_t)angle_index;
    return true;
}
