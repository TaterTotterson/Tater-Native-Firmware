#include "sat1_beamformer.h"

#include <string.h>

#define SAT1_BEAMFORMER_CENTER_DELAY_Q8 \
    ((DOA_ESTIMATOR_MAX_LAG_SAMPLES * DOA_ESTIMATOR_LAG_Q8_SCALE) / 2)
#define SAT1_BEAMFORMER_MAX_DELAY_Q8 \
    (DOA_ESTIMATOR_MAX_LAG_SAMPLES * DOA_ESTIMATOR_LAG_Q8_SCALE)
#define SAT1_BEAMFORMER_DIRECTION_HOLD_FRAMES (6)

static int16_t clamp_delay_q8(int32_t delay_q8)
{
    if (delay_q8 < 0) {
        return 0;
    }
    if (delay_q8 > SAT1_BEAMFORMER_MAX_DELAY_Q8) {
        return SAT1_BEAMFORMER_MAX_DELAY_Q8;
    }
    return (int16_t)delay_q8;
}

static int32_t sample_at(const int32_t *input,
                         const int32_t history[SAT1_BEAMFORMER_HISTORY_SAMPLES],
                         int index)
{
    if (index >= 0) {
        return input[index];
    }

    int history_index = SAT1_BEAMFORMER_HISTORY_SAMPLES + index;
    if (history_index < 0) {
        return 0;
    }
    return history[history_index];
}

static int32_t fractional_delay_sample(
    const int32_t *input,
    const int32_t history[SAT1_BEAMFORMER_HISTORY_SAMPLES],
    size_t sample_index,
    int delay_q8)
{
    int whole_samples = delay_q8 / DOA_ESTIMATOR_LAG_Q8_SCALE;
    int fraction_q8 = delay_q8 % DOA_ESTIMATOR_LAG_Q8_SCALE;
    int newest_index = (int)sample_index - whole_samples;
    int32_t newest = sample_at(input, history, newest_index);

    if (fraction_q8 == 0) {
        return newest;
    }

    int32_t older = sample_at(input, history, newest_index - 1);
    int64_t interpolated =
        ((int64_t)newest * (DOA_ESTIMATOR_LAG_Q8_SCALE - fraction_q8)) +
        ((int64_t)older * fraction_q8);
    return (int32_t)(interpolated / DOA_ESTIMATOR_LAG_Q8_SCALE);
}

static void update_history(
    sat1_beamformer_t *beamformer,
    const int32_t *microphones[SAT1_BEAMFORMER_MIC_COUNT],
    size_t frame_count)
{
    for (size_t mic = 0; mic < SAT1_BEAMFORMER_MIC_COUNT; mic++) {
        if (frame_count >= SAT1_BEAMFORMER_HISTORY_SAMPLES) {
            memcpy(beamformer->history[mic],
                   &microphones[mic][frame_count -
                                     SAT1_BEAMFORMER_HISTORY_SAMPLES],
                   sizeof(beamformer->history[mic]));
            continue;
        }

        size_t keep = SAT1_BEAMFORMER_HISTORY_SAMPLES - frame_count;
        memmove(beamformer->history[mic],
                &beamformer->history[mic][frame_count],
                keep * sizeof(int32_t));
        memcpy(&beamformer->history[mic][keep],
               microphones[mic],
               frame_count * sizeof(int32_t));
    }
}

static void select_target_delays(
    sat1_beamformer_t *beamformer,
    int16_t target_delay_q8[SAT1_BEAMFORMER_MIC_COUNT],
    const doa_estimator_state_t *direction)
{
    memcpy(target_delay_q8,
           beamformer->current_delay_q8,
           sizeof(beamformer->current_delay_q8));

    uint8_t valid = direction != NULL &&
                    (direction->flags & DOA_ESTIMATOR_FLAG_VALID) != 0 &&
                    (direction->flags & DOA_ESTIMATOR_FLAG_FOUR_MIC) != 0;
    if (valid) {
        int32_t half_ew_q8 = direction->sample_delay_q8 / 2;
        int32_t half_ns_q8 = direction->vertical_delay_q8 / 2;

        target_delay_q8[0] = clamp_delay_q8(
            SAT1_BEAMFORMER_CENTER_DELAY_Q8 + half_ew_q8);
        target_delay_q8[1] = clamp_delay_q8(
            SAT1_BEAMFORMER_CENTER_DELAY_Q8 - half_ew_q8);
        target_delay_q8[2] = clamp_delay_q8(
            SAT1_BEAMFORMER_CENTER_DELAY_Q8 + half_ns_q8);
        target_delay_q8[3] = clamp_delay_q8(
            SAT1_BEAMFORMER_CENTER_DELAY_Q8 - half_ns_q8);
        beamformer->invalid_direction_frames = 0;
        beamformer->has_direction = 1;
        return;
    }

    if (!beamformer->has_direction) {
        return;
    }

    if (beamformer->invalid_direction_frames < UINT8_MAX) {
        beamformer->invalid_direction_frames++;
    }
    if (beamformer->invalid_direction_frames <
        SAT1_BEAMFORMER_DIRECTION_HOLD_FRAMES) {
        return;
    }

    for (size_t mic = 0; mic < SAT1_BEAMFORMER_MIC_COUNT; mic++) {
        target_delay_q8[mic] = SAT1_BEAMFORMER_CENTER_DELAY_Q8;
    }
    beamformer->has_direction = 0;
}

void sat1_beamformer_reset(sat1_beamformer_t *beamformer)
{
    if (beamformer == NULL) {
        return;
    }

    memset(beamformer, 0, sizeof(*beamformer));
    for (size_t mic = 0; mic < SAT1_BEAMFORMER_MIC_COUNT; mic++) {
        beamformer->current_delay_q8[mic] =
            SAT1_BEAMFORMER_CENTER_DELAY_Q8;
    }
    beamformer->initialized = 1;
}

void sat1_beamformer_process_frame(
    sat1_beamformer_t *beamformer,
    int32_t *output,
    const int32_t *microphones[SAT1_BEAMFORMER_MIC_COUNT],
    size_t frame_count,
    const doa_estimator_state_t *direction)
{
    if (beamformer == NULL || output == NULL || microphones == NULL ||
        frame_count == 0) {
        return;
    }
    for (size_t mic = 0; mic < SAT1_BEAMFORMER_MIC_COUNT; mic++) {
        if (microphones[mic] == NULL) {
            return;
        }
    }

    if (!beamformer->initialized) {
        sat1_beamformer_reset(beamformer);
    }

    int16_t target_delay_q8[SAT1_BEAMFORMER_MIC_COUNT];
    select_target_delays(beamformer, target_delay_q8, direction);

    for (size_t sample = 0; sample < frame_count; sample++) {
        int64_t sum = 0;
        for (size_t mic = 0; mic < SAT1_BEAMFORMER_MIC_COUNT; mic++) {
            int32_t delta = target_delay_q8[mic] -
                            beamformer->current_delay_q8[mic];
            int delay_q8 = beamformer->current_delay_q8[mic] +
                           ((delta * (int32_t)(sample + 1)) /
                            (int32_t)frame_count);
            sum += fractional_delay_sample(
                microphones[mic],
                beamformer->history[mic],
                sample,
                delay_q8);
        }
        output[sample] = (int32_t)(sum / SAT1_BEAMFORMER_MIC_COUNT);
    }

    memcpy(beamformer->current_delay_q8,
           target_delay_q8,
           sizeof(beamformer->current_delay_q8));
    update_history(beamformer, microphones, frame_count);
}
