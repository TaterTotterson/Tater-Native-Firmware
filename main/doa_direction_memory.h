#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TATER_DOA_DIRECTION_MAX_LEDS (24U)

typedef struct {
    uint32_t votes[TATER_DOA_DIRECTION_MAX_LEDS];
    uint8_t led_count;
    uint8_t fallback_led;
    uint8_t dominant_led;
    bool valid;
} tater_doa_direction_memory_t;

void tater_doa_direction_memory_reset(
    tater_doa_direction_memory_t *memory,
    uint8_t led_count,
    uint8_t fallback_led
);
void tater_doa_direction_memory_observe(
    tater_doa_direction_memory_t *memory,
    float position,
    uint8_t confidence
);
bool tater_doa_direction_memory_dominant(
    const tater_doa_direction_memory_t *memory,
    uint8_t *led_out
);
