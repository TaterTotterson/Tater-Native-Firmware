#pragma once
#include "voicepe_audio_protocol.h"

typedef enum {
    DOA_SERVICER_CMD_READ_STATE = 0,
    DOA_SERVICER_CMD_READ_CAPABILITIES = VOICEPE_DOA_READ_CAPABILITIES,
    DOA_SERVICER_CMD_SET_HINTS = VOICEPE_DOA_SET_HINTS,
    NUM_DOA_SERVICER_CMDS
} doa_servicer_cmd_id_t;

/*
 * READ_STATE payload:
 *   bytes 0-1: int16 sample delay, little-endian
 *   byte  2  : confidence, 0-255
 *   byte  3  : flags, bit 0 means estimate is valid
 *   bytes 4-7: frame energy, little-endian
 *   bytes 8-11: estimator frame counter, little-endian
 */
#define DOA_SERVICER_STATE_NUM_VALUES (12)
