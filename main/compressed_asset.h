#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Expand a zlib-compressed embedded asset into PSRAM. The caller owns *out. */
esp_err_t tater_compressed_asset_expand(
    const uint8_t *compressed,
    size_t compressed_size,
    size_t expected_size,
    uint8_t **out
);
