#include "doa_activity_gate.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define DOA_ACTIVITY_CALIBRATION_FRAMES (8U)
#define DOA_ACTIVITY_ATTACK_FRAMES (2U)
#define DOA_ACTIVITY_RELEASE_FRAMES (3U)
#define DOA_ACTIVITY_MIN_NOISE_FLOOR (8U)
#define DOA_ACTIVITY_ATTACK_MARGIN (24U)
#define DOA_ACTIVITY_RELEASE_MARGIN (12U)
#define DOA_ACTIVITY_PEAK_MULTIPLIER (4U)

static uint32_t clamp_u32(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static uint32_t attack_threshold(uint32_t noise_floor)
{
    uint64_t scaled = ((uint64_t)noise_floor * 5U) / 2U;
    uint32_t margin = clamp_u32(
        (uint64_t)noise_floor + DOA_ACTIVITY_ATTACK_MARGIN
    );
    return max_u32(clamp_u32(scaled), margin);
}

static uint32_t release_threshold(uint32_t noise_floor)
{
    uint64_t scaled = ((uint64_t)noise_floor * 3U) / 2U;
    uint32_t margin = clamp_u32(
        (uint64_t)noise_floor + DOA_ACTIVITY_RELEASE_MARGIN
    );
    return max_u32(clamp_u32(scaled), margin);
}

static void update_noise_floor(tater_doa_activity_gate_t *gate, uint32_t mean_abs)
{
    uint32_t floor = gate->noise_floor;
    if (mean_abs < floor) {
        floor -= (floor - mean_abs + 3U) / 4U;
    } else if (mean_abs > floor) {
        floor += (mean_abs - floor + 31U) / 32U;
    }
    gate->noise_floor = max_u32(floor, DOA_ACTIVITY_MIN_NOISE_FLOOR);
}

void tater_doa_activity_gate_reset(tater_doa_activity_gate_t *gate)
{
    if (gate) {
        memset(gate, 0, sizeof(*gate));
    }
}

bool tater_doa_activity_gate_update(
    tater_doa_activity_gate_t *gate,
    uint32_t peak,
    uint32_t mean_abs
)
{
    if (!gate) {
        return false;
    }

    if (gate->calibration_frames < DOA_ACTIVITY_CALIBRATION_FRAMES) {
        uint32_t count = gate->calibration_frames;
        uint64_t total = ((uint64_t)gate->noise_floor * count) + mean_abs;
        gate->calibration_frames++;
        gate->noise_floor = max_u32(
            clamp_u32(total / gate->calibration_frames),
            DOA_ACTIVITY_MIN_NOISE_FLOOR
        );
        gate->attack_frames = 0;
        gate->release_frames = 0;
        gate->active = false;
        return false;
    }

    uint32_t attack = attack_threshold(gate->noise_floor);
    uint32_t release = release_threshold(gate->noise_floor);
    uint32_t peak_threshold = clamp_u32((uint64_t)attack * DOA_ACTIVITY_PEAK_MULTIPLIER);
    bool signal = mean_abs >= attack ||
                  (mean_abs >= release && peak >= peak_threshold);

    if (!gate->active) {
        gate->release_frames = 0;
        if (signal) {
            if (gate->attack_frames < UINT8_MAX) {
                gate->attack_frames++;
            }
            if (gate->attack_frames >= DOA_ACTIVITY_ATTACK_FRAMES) {
                gate->active = true;
                gate->attack_frames = 0;
            }
        } else {
            gate->attack_frames = 0;
            update_noise_floor(gate, mean_abs);
        }
        return gate->active;
    }

    gate->attack_frames = 0;
    if (mean_abs < release) {
        if (gate->release_frames < UINT8_MAX) {
            gate->release_frames++;
        }
        if (gate->release_frames >= DOA_ACTIVITY_RELEASE_FRAMES) {
            gate->active = false;
            gate->release_frames = 0;
            update_noise_floor(gate, mean_abs);
        }
    } else {
        gate->release_frames = 0;
    }
    return gate->active;
}

uint32_t tater_doa_activity_gate_noise_floor(const tater_doa_activity_gate_t *gate)
{
    return gate ? gate->noise_floor : 0;
}
