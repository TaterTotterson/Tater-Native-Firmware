#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool run_active;
    uint64_t run_start_sample;
    uint64_t endpoint_sample;
    uint64_t peak_sample;
    float peak_score;
} tater_wake_boundary_policy_t;

/* Select the leading threshold crossing of the strongest confidence run.
 * A model can keep rising or remain saturated after the acoustic wake end.
 * Moving the endpoint to that later peak discards immediate command speech.
 * A separate, weaker early blip must not displace the strongest event. */
static inline void tater_wake_boundary_note(
    tater_wake_boundary_policy_t *state,
    uint64_t sample,
    float score,
    float threshold,
    uint64_t window_start,
    uint64_t window_end
)
{
    if (sample < window_start || sample > window_end || !(score >= threshold)) {
        state->run_active = false;
        return;
    }
    if (!state->run_active) {
        state->run_active = true;
        state->run_start_sample = sample;
    }
    if (score > state->peak_score) {
        state->peak_score = score;
        state->peak_sample = sample;
        state->endpoint_sample = state->run_start_sample;
    }
}
