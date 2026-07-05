#include "wake_model_assets.h"

#include <string.h>

extern const uint8_t _binary_hey_tater_tflite_start[] asm("_binary_hey_tater_tflite_start");
extern const uint8_t _binary_hey_tater_tflite_end[] asm("_binary_hey_tater_tflite_end");

#define WAKE_MODEL_ASSET(asset_id, asset_label, symbol_name) \
    { asset_id, asset_label, _binary_##symbol_name##_tflite_start, _binary_##symbol_name##_tflite_end }

static const tater_wake_model_asset_t s_assets[] = {
    WAKE_MODEL_ASSET("hey_tater", "Hey Tater", hey_tater),
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
