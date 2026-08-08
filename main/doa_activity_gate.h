#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t noise_floor;
    uint8_t calibration_frames;
    uint8_t attack_frames;
    uint8_t release_frames;
    bool active;
} tater_doa_activity_gate_t;

void tater_doa_activity_gate_reset(tater_doa_activity_gate_t *gate);
bool tater_doa_activity_gate_update(
    tater_doa_activity_gate_t *gate,
    uint32_t peak,
    uint32_t mean_abs
);
uint32_t tater_doa_activity_gate_noise_floor(const tater_doa_activity_gate_t *gate);
