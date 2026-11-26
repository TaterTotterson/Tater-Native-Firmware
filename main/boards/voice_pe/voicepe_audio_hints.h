#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "xmos/voicepe_audio_protocol.h"

typedef struct {
    bool supported;
    bool sent;
    uint8_t last_flags;
    uint32_t last_send_ms;
} voicepe_audio_hints_t;

void voicepe_audio_hints_init(voicepe_audio_hints_t *state,
                            const uint8_t capabilities[4]);
bool voicepe_audio_hints_due(const voicepe_audio_hints_t *state,
                            uint8_t flags, uint32_t now_ms);
void voicepe_audio_hints_sent(voicepe_audio_hints_t *state,
                             uint8_t flags, uint32_t now_ms);
