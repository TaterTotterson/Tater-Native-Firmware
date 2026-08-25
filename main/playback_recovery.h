#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TATER_PLAYBACK_RECOVERY_NONE = 0,
    TATER_PLAYBACK_RECOVERY_OUTPUT_STALLED,
    TATER_PLAYBACK_RECOVERY_REBUFFER_STALLED,
} tater_playback_recovery_action_t;

typedef struct {
    bool output_tracking;
    bool rebuffer_tracking;
    bool recovery_fired;
    uint32_t last_completed_frames;
    uint32_t last_output_progress_ms;
    uint32_t rebuffer_started_ms;
} tater_playback_recovery_state_t;

void tater_playback_recovery_reset(tater_playback_recovery_state_t *state);

void tater_playback_recovery_note_output(
    tater_playback_recovery_state_t *state,
    uint32_t completed_frames,
    uint32_t now_ms
);

void tater_playback_recovery_stop_output(
    tater_playback_recovery_state_t *state
);

tater_playback_recovery_action_t tater_playback_recovery_observe_output(
    tater_playback_recovery_state_t *state,
    uint32_t completed_frames,
    uint32_t now_ms,
    uint32_t timeout_ms
);

tater_playback_recovery_action_t tater_playback_recovery_observe_rebuffer(
    tater_playback_recovery_state_t *state,
    bool rebuffering,
    bool recovery_allowed,
    uint32_t now_ms,
    uint32_t timeout_ms
);
