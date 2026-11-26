#include "voicepe_audio_hints.h"
#include <string.h>

void voicepe_audio_hints_init(voicepe_audio_hints_t *state,
                            const uint8_t capabilities[4])
{
    memset(state, 0, sizeof(*state));
    state->supported = capabilities != NULL &&
        capabilities[0] == 'V' && capabilities[1] == 'P' &&
        capabilities[2] == VOICEPE_DOA_PROTOCOL_VERSION &&
        (capabilities[3] & (VOICEPE_DOA_CAP_POST_AEC | VOICEPE_DOA_CAP_HOST_HINTS)) ==
            (VOICEPE_DOA_CAP_POST_AEC | VOICEPE_DOA_CAP_HOST_HINTS);
}

bool voicepe_audio_hints_due(const voicepe_audio_hints_t *state,
                            uint8_t flags, uint32_t now_ms)
{
    return state->supported && (!state->sent ||
        state->last_flags != (flags & VOICEPE_DOA_HINT_MASK) ||
        (uint32_t)(now_ms - state->last_send_ms) >= VOICEPE_DOA_HINT_REFRESH_MS);
}

void voicepe_audio_hints_sent(voicepe_audio_hints_t *state,
                             uint8_t flags, uint32_t now_ms)
{
    state->sent = true;
    state->last_flags = flags & VOICEPE_DOA_HINT_MASK;
    state->last_send_ms = now_ms;
}
