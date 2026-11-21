#include "wake_boundary_policy.h"

#include <assert.h>
#include <stdio.h>

static void note(tater_wake_boundary_policy_t *state, uint64_t ms, float score)
{
    tater_wake_boundary_note(state, ms * 16, score, 0.82f, 2300 * 16, 3120 * 16);
}

int main(void)
{
    tater_wake_boundary_policy_t state = {0};
    /* Scores replayed from the clipped Sat1 command. Peak selection placed
     * the guarded cut at 2870 ms, inside "what". The same event's leading
     * crossing places it at 2810 ms; replaying that audio restores the word. */
    note(&state, 2870, 0.5f);
    note(&state, 2890, 0.8863f);
    note(&state, 2910, 0.9686f);
    note(&state, 2930, 0.9922f);
    note(&state, 2950, 1.0f);
    note(&state, 2970, 1.0f);
    assert(state.endpoint_sample == 2890 * 16);
    assert(state.peak_sample == 2950 * 16);
    assert(state.peak_score == 1.0f);
    assert(state.endpoint_sample - 80 * 16 == 2810 * 16);

    /* Choose the strongest event, not an unrelated earlier threshold blip. */
    state = (tater_wake_boundary_policy_t){0};
    note(&state, 2400, 0.85f);
    note(&state, 2420, 0.2f);
    note(&state, 2800, 0.86f);
    note(&state, 2820, 0.99f);
    assert(state.endpoint_sample == 2800 * 16);
    assert(state.peak_sample == 2820 * 16);
    note(&state, 2840, 0.1f);
    note(&state, 2860, 0.9f);
    assert(state.endpoint_sample == 2800 * 16);

    /* Keep search bounds and threshold rejection; equal peaks keep the
     * earlier event, and a run crossing the window cannot rewind outside it. */
    state = (tater_wake_boundary_policy_t){0};
    note(&state, 2280, 1.0f);
    note(&state, 2300, 0.9f);
    note(&state, 2320, 1.0f);
    note(&state, 2340, 0.1f);
    note(&state, 2800, 1.0f);
    note(&state, 3140, 1.0f);
    assert(state.endpoint_sample == 2300 * 16);
    assert(state.peak_sample == 2320 * 16);
    state = (tater_wake_boundary_policy_t){0};
    note(&state, 2500, 0.819f);
    note(&state, 3000, 0.1f);
    assert(state.endpoint_sample == 0);

    puts("Wake boundary confidence-run host tests passed");
    return 0;
}
