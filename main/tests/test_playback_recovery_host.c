#include <assert.h>
#include <stdio.h>

#include "../playback_recovery.h"

static void test_output_progress_and_stall(void)
{
    tater_playback_recovery_state_t state;
    tater_playback_recovery_reset(&state);

    assert(
        tater_playback_recovery_observe_output(&state, 0, 5000, 5000)
        == TATER_PLAYBACK_RECOVERY_NONE
    );
    tater_playback_recovery_note_output(&state, 100, 1000);
    assert(
        tater_playback_recovery_observe_output(&state, 200, 5000, 5000)
        == TATER_PLAYBACK_RECOVERY_NONE
    );
    assert(
        tater_playback_recovery_observe_output(&state, 200, 9999, 5000)
        == TATER_PLAYBACK_RECOVERY_NONE
    );
    assert(
        tater_playback_recovery_observe_output(&state, 200, 10000, 5000)
        == TATER_PLAYBACK_RECOVERY_OUTPUT_STALLED
    );
    assert(
        tater_playback_recovery_observe_output(&state, 200, 20000, 5000)
        == TATER_PLAYBACK_RECOVERY_NONE
    );
}

static void test_output_stop_and_session_reset(void)
{
    tater_playback_recovery_state_t state;
    tater_playback_recovery_reset(&state);
    tater_playback_recovery_note_output(&state, 50, 1000);
    tater_playback_recovery_stop_output(&state);
    assert(
        tater_playback_recovery_observe_output(&state, 50, 10000, 5000)
        == TATER_PLAYBACK_RECOVERY_NONE
    );

    tater_playback_recovery_note_output(&state, 75, 11000);
    assert(
        tater_playback_recovery_observe_output(&state, 75, 16000, 5000)
        == TATER_PLAYBACK_RECOVERY_OUTPUT_STALLED
    );
    tater_playback_recovery_reset(&state);
    tater_playback_recovery_note_output(&state, 10, 20000);
    assert(
        tater_playback_recovery_observe_output(&state, 10, 24999, 5000)
        == TATER_PLAYBACK_RECOVERY_NONE
    );
}

static void test_rebuffer_timeout_and_pause(void)
{
    tater_playback_recovery_state_t state;
    tater_playback_recovery_reset(&state);

    assert(
        tater_playback_recovery_observe_rebuffer(&state, true, true, 1000, 5000)
        == TATER_PLAYBACK_RECOVERY_NONE
    );
    assert(
        tater_playback_recovery_observe_rebuffer(&state, true, false, 5000, 5000)
        == TATER_PLAYBACK_RECOVERY_NONE
    );
    assert(
        tater_playback_recovery_observe_rebuffer(&state, true, true, 6000, 5000)
        == TATER_PLAYBACK_RECOVERY_NONE
    );
    assert(
        tater_playback_recovery_observe_rebuffer(&state, true, true, 10999, 5000)
        == TATER_PLAYBACK_RECOVERY_NONE
    );
    assert(
        tater_playback_recovery_observe_rebuffer(&state, true, true, 11000, 5000)
        == TATER_PLAYBACK_RECOVERY_REBUFFER_STALLED
    );
}

int main(void)
{
    test_output_progress_and_stall();
    test_output_stop_and_session_reset();
    test_rebuffer_timeout_and_pause();
    puts("playback recovery host tests passed");
    return 0;
}
