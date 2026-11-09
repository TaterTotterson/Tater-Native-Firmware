#include "wake_model_assets.h"

#include <string.h>

extern const uint8_t _binary_hey_tater_tflite_start[] asm("_binary_hey_tater_tflite_start");
extern const uint8_t _binary_hey_tater_tflite_end[] asm("_binary_hey_tater_tflite_end");
extern const uint8_t _binary_hey_tater_boundary_tflite_start[] asm("_binary_hey_tater_boundary_tflite_start");
extern const uint8_t _binary_hey_tater_boundary_tflite_end[] asm("_binary_hey_tater_boundary_tflite_end");
extern const uint8_t _binary_stop_tflite_start[] asm("_binary_stop_tflite_start");
extern const uint8_t _binary_stop_tflite_end[] asm("_binary_stop_tflite_end");

#define WAKE_MODEL_ASSET(asset_id, asset_label, symbol_name) \
    { asset_id, asset_label, _binary_##symbol_name##_tflite_start, _binary_##symbol_name##_tflite_end }

static const tater_wake_model_asset_t s_assets[] = {
    WAKE_MODEL_ASSET("hey_tater", "Hey Tater", hey_tater),
    WAKE_MODEL_ASSET("stop", "Stop", stop),
};

static const tater_wake_model_asset_t s_boundary_asset = {
    "hey_tater_boundary",
    "Hey Tater Boundary",
    _binary_hey_tater_boundary_tflite_start,
    _binary_hey_tater_boundary_tflite_end,
};

const tater_wake_model_asset_t *tater_wake_model_asset_lookup(const char *id)
{
    if (!id || id[0] == '\0' || strcmp(id, "default") == 0) {
        id = "hey_tater";
    }
    for (size_t i = 0; i < (sizeof(s_assets) / sizeof(s_assets[0])); i++) {
        if (strcmp(id, s_assets[i].id) == 0) {
            return &s_assets[i];
        }
    }
    return NULL;
}

const tater_wake_model_asset_t *tater_wake_boundary_asset(void)
{
    return &s_boundary_asset;
}
