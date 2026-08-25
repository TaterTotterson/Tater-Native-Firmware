#include "playback_recovery.h"

#include <string.h>

void tater_playback_recovery_reset(tater_playback_recovery_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

void tater_playback_recovery_note_output(
    tater_playback_recovery_state_t *state,
    uint32_t completed_frames,
    uint32_t now_ms
)
{
    if (!state || state->recovery_fired) {
        return;
    }
    if (!state->output_tracking) {
        state->output_tracking = true;
        state->last_completed_frames = completed_frames;
        state->last_output_progress_ms = now_ms;
        return;
    }
    if (completed_frames != state->last_completed_frames) {
        state->last_completed_frames = completed_frames;
        state->last_output_progress_ms = now_ms;
    }
}

void tater_playback_recovery_stop_output(
    tater_playback_recovery_state_t *state
)
{
    if (!state) {
        return;
    }
    state->output_tracking = false;
}

tater_playback_recovery_action_t tater_playback_recovery_observe_output(
    tater_playback_recovery_state_t *state,
    uint32_t completed_frames,
    uint32_t now_ms,
    uint32_t timeout_ms
)
{
    if (!state || !state->output_tracking || state->recovery_fired) {
        return TATER_PLAYBACK_RECOVERY_NONE;
    }
    if (completed_frames != state->last_completed_frames) {
        state->last_completed_frames = completed_frames;
        state->last_output_progress_ms = now_ms;
        return TATER_PLAYBACK_RECOVERY_NONE;
    }
    if (timeout_ms == 0 || (uint32_t)(now_ms - state->last_output_progress_ms) < timeout_ms) {
        return TATER_PLAYBACK_RECOVERY_NONE;
    }
    state->recovery_fired = true;
    return TATER_PLAYBACK_RECOVERY_OUTPUT_STALLED;
}

tater_playback_recovery_action_t tater_playback_recovery_observe_rebuffer(
    tater_playback_recovery_state_t *state,
    bool rebuffering,
    bool recovery_allowed,
    uint32_t now_ms,
    uint32_t timeout_ms
)
{
    if (!state) {
        return TATER_PLAYBACK_RECOVERY_NONE;
    }
    if (!rebuffering || !recovery_allowed || state->recovery_fired) {
        state->rebuffer_tracking = false;
        return TATER_PLAYBACK_RECOVERY_NONE;
    }
    if (!state->rebuffer_tracking) {
        state->rebuffer_tracking = true;
        state->rebuffer_started_ms = now_ms;
        return TATER_PLAYBACK_RECOVERY_NONE;
    }
    if (timeout_ms == 0 || (uint32_t)(now_ms - state->rebuffer_started_ms) < timeout_ms) {
        return TATER_PLAYBACK_RECOVERY_NONE;
    }
    state->recovery_fired = true;
    return TATER_PLAYBACK_RECOVERY_REBUFFER_STALLED;
}
