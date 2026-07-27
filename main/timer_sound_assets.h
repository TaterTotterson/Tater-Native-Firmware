#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    const uint8_t *end;
} tater_timer_sound_asset_t;

const tater_timer_sound_asset_t *tater_timer_sound_asset(void);
