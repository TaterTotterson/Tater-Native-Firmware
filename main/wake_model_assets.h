#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *id;
    const char *label;
    const uint8_t *data;
    const uint8_t *end;
} tater_wake_model_asset_t;

const tater_wake_model_asset_t *tater_wake_model_asset_lookup(const char *id);
