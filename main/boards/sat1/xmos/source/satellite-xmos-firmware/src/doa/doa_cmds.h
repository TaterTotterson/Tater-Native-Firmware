#pragma once

typedef enum {
    DOA_SERVICER_CMD_READ_STATE = 0,
    NUM_DOA_SERVICER_CMDS
} doa_servicer_cmd_id_t;

/*
 * READ_STATE payload:
 *   bytes 0-1: int16 sample delay, little-endian
 *   byte  2  : confidence, 0-255
 *   byte  3  : flags, bit 0 means estimate is valid
 *   bytes 4-7: frame energy, little-endian
 *   bytes 8-11: estimator frame counter, little-endian
 *   bytes 12-13: int16 vertical delay, little-endian
 *   byte  14: angle index for the ESP LED ring
 *   byte  15: reserved
 *   bytes 16-31: four uint32 mic energy values, little-endian
 */
#define DOA_SERVICER_STATE_NUM_VALUES (32)
