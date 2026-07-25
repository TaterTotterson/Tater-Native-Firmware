#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t degrees;
    bool speech_detected;
    uint8_t angle_index;
} xvf3800_doa_value_t;

bool xvf3800_doa_decode(const uint8_t *response,
                        size_t response_len,
                        uint8_t led_count,
                        int led_offset,
                        xvf3800_doa_value_t *out);
