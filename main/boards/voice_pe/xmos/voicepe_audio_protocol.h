#pragma once

/* Resource 231 keeps its original 12-byte direction response. New commands
 * are used only after the host recognizes the capability signature. */
#define VOICEPE_DOA_RESOURCE 231
#define VOICEPE_DOA_READ_STATE 0
#define VOICEPE_DOA_READ_CAPABILITIES 1
#define VOICEPE_DOA_SET_HINTS 2
#define VOICEPE_DOA_STATE_BYTES 12
#define VOICEPE_DOA_CAPABILITY_BYTES 4
#define VOICEPE_DOA_PROTOCOL_VERSION 1
#define VOICEPE_DOA_CAP_POST_AEC 1u
#define VOICEPE_DOA_CAP_HOST_HINTS 2u
#define VOICEPE_DOA_HINT_PLAYBACK 1u
#define VOICEPE_DOA_HINT_LISTENING 2u
#define VOICEPE_DOA_HINT_MASK 3u
#define VOICEPE_DOA_HINT_TTL_MS 750u
#define VOICEPE_DOA_HINT_REFRESH_MS 250u

/* READ_STATE flags. Bit zero retains its original meaning. */
#define VOICEPE_DOA_FLAG_VALID 1u
#define VOICEPE_DOA_FLAG_SPEECH 2u
#define VOICEPE_DOA_FLAG_PLAYBACK 4u
#define VOICEPE_DOA_FLAG_LISTENING 8u
#define VOICEPE_DOA_FLAG_HINTS_FRESH 16u
