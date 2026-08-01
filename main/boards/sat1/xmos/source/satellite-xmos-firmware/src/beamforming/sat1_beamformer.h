#pragma once

#include <stddef.h>
#include <stdint.h>

#include "doa/doa_estimator.h"

#define SAT1_BEAMFORMER_MIC_COUNT (4)
#define SAT1_BEAMFORMER_HISTORY_SAMPLES \
    (DOA_ESTIMATOR_MAX_LAG_SAMPLES + 1)

typedef struct {
    int32_t history[SAT1_BEAMFORMER_MIC_COUNT]
                   [SAT1_BEAMFORMER_HISTORY_SAMPLES];
    int16_t current_delay_q8[SAT1_BEAMFORMER_MIC_COUNT];
    uint8_t invalid_direction_frames;
    uint8_t has_direction;
    uint8_t initialized;
} sat1_beamformer_t;

/*
 * Four-microphone fractional-delay delay-and-sum beamformer.
 *
 * Microphone order is East, West, North, South. The DoA estimator supplies
 * the East/West and North/South pair delays in Q8 samples. A fixed center
 * delay keeps total latency constant while steering changes.
 */
void sat1_beamformer_process_frame(
    sat1_beamformer_t *beamformer,
    int32_t *output,
    const int32_t *microphones[SAT1_BEAMFORMER_MIC_COUNT],
    size_t frame_count,
    const doa_estimator_state_t *direction);

void sat1_beamformer_reset(sat1_beamformer_t *beamformer);
