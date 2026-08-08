#include "doa_direction_memory.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static void make_vote_room(tater_doa_direction_memory_t *memory, uint32_t amount)
{
    for (uint8_t led = 0; led < memory->led_count; led++) {
        if (memory->votes[led] > UINT32_MAX - amount) {
            for (uint8_t shrink = 0; shrink < memory->led_count; shrink++) {
                memory->votes[shrink] >>= 1;
            }
            return;
        }
    }
}

void tater_doa_direction_memory_reset(
    tater_doa_direction_memory_t *memory,
    uint8_t led_count,
    uint8_t fallback_led
)
{
    if (!memory) {
        return;
    }
    memset(memory, 0, sizeof(*memory));
    if (led_count == 0 || led_count > TATER_DOA_DIRECTION_MAX_LEDS) {
        return;
    }
    memory->led_count = led_count;
    memory->fallback_led = fallback_led % led_count;
    memory->dominant_led = memory->fallback_led;
}

void tater_doa_direction_memory_observe(
    tater_doa_direction_memory_t *memory,
    float position,
    uint8_t confidence
)
{
    if (!memory || memory->led_count == 0) {
        return;
    }

    float led_count = (float)memory->led_count;
    while (position < 0.0f) {
        position += led_count;
    }
    while (position >= led_count) {
        position -= led_count;
    }

    uint8_t target = (uint8_t)(position + 0.5f) % memory->led_count;
    uint8_t bounded_confidence = confidence > 64U ? 64U : confidence;
    uint32_t weight = 256U + bounded_confidence;
    uint32_t neighbor_weight = weight / 4U;
    make_vote_room(memory, weight + (neighbor_weight * 2U));

    memory->votes[target] += weight;
    memory->votes[(target + 1U) % memory->led_count] += neighbor_weight;
    memory->votes[(target + memory->led_count - 1U) % memory->led_count] +=
        neighbor_weight;

    uint8_t best = memory->valid ? memory->dominant_led : target;
    uint32_t best_votes = memory->votes[best];
    for (uint8_t led = 0; led < memory->led_count; led++) {
        if (memory->votes[led] > best_votes) {
            best = led;
            best_votes = memory->votes[led];
        }
    }
    memory->dominant_led = best;
    memory->valid = true;
}

bool tater_doa_direction_memory_dominant(
    const tater_doa_direction_memory_t *memory,
    uint8_t *led_out
)
{
    if (!memory || memory->led_count == 0) {
        return false;
    }
    if (led_out) {
        *led_out = memory->valid ? memory->dominant_led : memory->fallback_led;
    }
    return memory->valid;
}
