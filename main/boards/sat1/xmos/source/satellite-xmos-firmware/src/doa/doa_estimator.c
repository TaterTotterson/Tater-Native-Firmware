#include "doa_estimator.h"

#include <limits.h>
#include <string.h>

#define DOA_ESTIMATOR_SAMPLE_SHIFT (12)
#define DOA_ESTIMATOR_MIN_ENERGY   (4096)
#define DOA_ESTIMATOR_SCORE_SCALE  (65535U)
#define DOA_ESTIMATOR_MIN_PEAK_SCORE (12000U)
#define DOA_ESTIMATOR_LAG_Q8_SCALE (256)
#define DOA_ESTIMATOR_MIN_VECTOR_Q8 (48)
#define DOA_ESTIMATOR_HISTORY_SAMPLES (480)
#define DOA_ESTIMATOR_SMOOTH_NUMERATOR (3)
#define DOA_ESTIMATOR_SMOOTH_DENOMINATOR (8)
#define DOA_ESTIMATOR_INVALID_RESET_FRAMES (6)
#define DOA_ESTIMATOR_NOISE_CALIBRATION_FRAMES (8)
#define DOA_ESTIMATOR_NOISE_FLOOR_MIN (64U)
#define DOA_ESTIMATOR_SIGNAL_ATTACK_MULTIPLIER (3U)
#define DOA_ESTIMATOR_SIGNAL_RELEASE_MULTIPLIER (2U)
#define DOA_ESTIMATOR_SIGNAL_MARGIN (64U)
#define DOA_ESTIMATOR_MIN_DIRECTION_CONFIDENCE (12U)

static volatile doa_estimator_state_t latest_state = {0};
static int32_t four_mic_history[4][DOA_ESTIMATOR_HISTORY_SAMPLES];
static size_t four_mic_history_count;
static int32_t smoothed_ew_delay_q8;
static int32_t smoothed_ns_delay_q8;
static uint8_t smoothed_direction_valid;
static uint8_t invalid_direction_frames;
static uint32_t noise_floor_energy;
static uint8_t noise_calibration_frames;
static uint8_t signal_energy_active;

typedef struct {
    int lag;
    int lag_q8;
    uint16_t peak_score;
    uint8_t confidence;
    uint8_t valid;
} pair_estimate_t;

static int abs_int(int value)
{
    return value < 0 ? -value : value;
}

static int32_t scaled_sample(int32_t sample)
{
    return sample >> DOA_ESTIMATOR_SAMPLE_SHIFT;
}

static uint16_t lag_score(const int32_t *mic0,
                          const int32_t *mic1,
                          size_t frame_count,
                          int lag)
{
    size_t start0 = 0;
    size_t start1 = 0;
    size_t count = frame_count;

    if (lag > 0) {
        start1 = (size_t)lag;
        count -= (size_t)lag;
    } else if (lag < 0) {
        start0 = (size_t)-lag;
        count -= (size_t)-lag;
    }

    if (count < 2) {
        return 0;
    }

    int64_t sum0 = 0;
    int64_t sum1 = 0;
    for (size_t i = 0; i < count; i++) {
        sum0 += scaled_sample(mic0[start0 + i]);
        sum1 += scaled_sample(mic1[start1 + i]);
    }

    int32_t mean0 = (int32_t)(sum0 / (int64_t)count);
    int32_t mean1 = (int32_t)(sum1 / (int64_t)count);
    int64_t corr = 0;
    int64_t energy0 = 0;
    int64_t energy1 = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t sample0 = scaled_sample(mic0[start0 + i]) - mean0;
        int32_t sample1 = scaled_sample(mic1[start1 + i]) - mean1;
        corr += (int64_t)sample0 * (int64_t)sample1;
        energy0 += (int64_t)sample0 * (int64_t)sample0;
        energy1 += (int64_t)sample1 * (int64_t)sample1;
    }

    if (corr <= 0 || energy0 <= 0 || energy1 <= 0) {
        return 0;
    }

    /*
     * Normalize correlation with the arithmetic mean of both channel
     * energies. This is bounded, gain tolerant, and avoids a square root on
     * xCORE while removing the zero-lag bias of the previous raw dot product.
     */
    uint64_t corr_u = (uint64_t)corr;
    uint64_t denominator = (uint64_t)energy0 + (uint64_t)energy1;
    const uint64_t multiplier = 2U * DOA_ESTIMATOR_SCORE_SCALE;
    while (corr_u > UINT64_MAX / multiplier) {
        corr_u >>= 1;
        denominator >>= 1;
    }
    if (denominator == 0) {
        return 0;
    }

    uint64_t score = ((corr_u * multiplier) + (denominator / 2U)) / denominator;
    if (score > DOA_ESTIMATOR_SCORE_SCALE) {
        score = DOA_ESTIMATOR_SCORE_SCALE;
    }
    return (uint16_t)score;
}

static uint32_t clamp_u32(int64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint32_t mic_energy(const int32_t *mic, size_t frame_count)
{
    int64_t energy = 0;

    if (mic == NULL) {
        return 0;
    }

    for (size_t i = 0; i < frame_count; i++) {
        int32_t sample = scaled_sample(mic[i]);
        energy += (int64_t)sample * (int64_t)sample;
    }

    return clamp_u32(energy);
}

static uint32_t mean_four_mic_energy(const uint32_t mic_energy_values[4],
                                     size_t frame_count)
{
    if (frame_count == 0) {
        return 0;
    }

    uint64_t total = (uint64_t)mic_energy_values[0] +
                     (uint64_t)mic_energy_values[1] +
                     (uint64_t)mic_energy_values[2] +
                     (uint64_t)mic_energy_values[3];
    uint64_t mean = total / ((uint64_t)frame_count * 4U);
    return mean > UINT32_MAX ? UINT32_MAX : (uint32_t)mean;
}

static void update_noise_floor(uint32_t frame_energy, uint32_t divisor)
{
    if (frame_energy > noise_floor_energy) {
        uint32_t step = (frame_energy - noise_floor_energy) / divisor;
        noise_floor_energy += step > 0 ? step : 1;
    } else {
        uint32_t step = (noise_floor_energy - frame_energy) / divisor;
        noise_floor_energy -= step > 0 ? step : (noise_floor_energy > frame_energy);
    }

    if (noise_floor_energy < DOA_ESTIMATOR_NOISE_FLOOR_MIN) {
        noise_floor_energy = DOA_ESTIMATOR_NOISE_FLOOR_MIN;
    }
}

static uint8_t four_mic_signal_present(uint32_t frame_energy)
{
    if (noise_calibration_frames < DOA_ESTIMATOR_NOISE_CALIBRATION_FRAMES) {
        if (noise_calibration_frames == 0) {
            noise_floor_energy = frame_energy;
        } else {
            uint32_t sample_count = (uint32_t)noise_calibration_frames + 1U;
            if (frame_energy >= noise_floor_energy) {
                noise_floor_energy +=
                    (frame_energy - noise_floor_energy) / sample_count;
            } else {
                noise_floor_energy -=
                    (noise_floor_energy - frame_energy) / sample_count;
            }
        }
        if (noise_floor_energy < DOA_ESTIMATOR_NOISE_FLOOR_MIN) {
            noise_floor_energy = DOA_ESTIMATOR_NOISE_FLOOR_MIN;
        }
        noise_calibration_frames++;
        signal_energy_active = 0;
        return 0;
    }

    uint32_t multiplier = signal_energy_active
                              ? DOA_ESTIMATOR_SIGNAL_RELEASE_MULTIPLIER
                              : DOA_ESTIMATOR_SIGNAL_ATTACK_MULTIPLIER;
    uint64_t threshold =
        ((uint64_t)noise_floor_energy * multiplier) +
        DOA_ESTIMATOR_SIGNAL_MARGIN;
    uint8_t present = frame_energy >= threshold;

    /*
     * Only quiet frames update the room-noise estimate. This keeps a long or
     * loud utterance from teaching the gate that speech itself is silence.
     */
    if (!present) {
        update_noise_floor(frame_energy, 8U);
    }
    signal_energy_active = present;
    return present;
}

static uint8_t estimate_angle_index(int ew_delay_q8, int ns_delay_q8)
{
    static const int16_t led_x[24] = {
        0, -259, -500, -707, -866, -966, -1000, -966,
        -866, -707, -500, -259, 0, 259, 500, 707,
        866, 966, 1000, 966, 866, 707, 500, 259,
    };
    static const int16_t led_y[24] = {
        -1000, -966, -866, -707, -500, -259, 0, 259,
        500, 707, 866, 966, 1000, 966, 866, 707,
        500, 259, 0, -259, -500, -707, -866, -966,
    };
    int best_index = 12;
    int32_t best_dot = INT32_MIN;

    if (ew_delay_q8 == 0 && ns_delay_q8 == 0) {
        return 12;
    }

    for (int i = 0; i < 24; i++) {
        int32_t dot = (ew_delay_q8 * led_x[i]) + (ns_delay_q8 * led_y[i]);
        if (dot > best_dot) {
            best_dot = dot;
            best_index = i;
        }
    }

    return (uint8_t)best_index;
}

static pair_estimate_t estimate_pair(const int32_t *mic0,
                                     const int32_t *mic1,
                                     size_t frame_count)
{
    pair_estimate_t estimate = {0};
    uint16_t scores[(DOA_ESTIMATOR_MAX_LAG_SAMPLES * 2) + 1] = {0};
    uint16_t best_score = 0;
    int best_index = DOA_ESTIMATOR_MAX_LAG_SAMPLES;

    if (mic0 == NULL || mic1 == NULL || frame_count <= (2 * DOA_ESTIMATOR_MAX_LAG_SAMPLES)) {
        return estimate;
    }

    for (int lag = -DOA_ESTIMATOR_MAX_LAG_SAMPLES;
         lag <= DOA_ESTIMATOR_MAX_LAG_SAMPLES;
         lag++) {
        int index = lag + DOA_ESTIMATOR_MAX_LAG_SAMPLES;
        scores[index] = lag_score(mic0, mic1, frame_count, lag);
        if (scores[index] > best_score) {
            best_score = scores[index];
            best_index = index;
        }
    }

    if (best_score < DOA_ESTIMATOR_MIN_PEAK_SCORE) {
        return estimate;
    }

    int best_lag = best_index - DOA_ESTIMATOR_MAX_LAG_SAMPLES;
    uint16_t second_score = 0;
    for (int index = 0; index < (int)(sizeof(scores) / sizeof(scores[0])); index++) {
        int lag = index - DOA_ESTIMATOR_MAX_LAG_SAMPLES;
        if (abs_int(lag - best_lag) <= 1) {
            continue;
        }
        if (scores[index] > second_score) {
            second_score = scores[index];
        }
    }

    uint32_t strength_confidence =
        ((uint32_t)(best_score - DOA_ESTIMATOR_MIN_PEAK_SCORE) * 255U) /
        (DOA_ESTIMATOR_SCORE_SCALE - DOA_ESTIMATOR_MIN_PEAK_SCORE);
    uint32_t separation_confidence =
        ((uint32_t)(best_score - second_score) * 255U) / best_score;
    uint32_t confidence = (strength_confidence + (2U * separation_confidence)) / 3U;

    int lag_q8 = best_lag * DOA_ESTIMATOR_LAG_Q8_SCALE;
    if (best_index > 0 &&
        best_index < (int)(sizeof(scores) / sizeof(scores[0])) - 1) {
        int32_t left = scores[best_index - 1];
        int32_t center = scores[best_index];
        int32_t right = scores[best_index + 1];
        int32_t curvature = left - (2 * center) + right;
        if (curvature < 0) {
            int32_t delta_q8 =
                (128 * (left - right)) / curvature;
            if (delta_q8 > 128) {
                delta_q8 = 128;
            } else if (delta_q8 < -128) {
                delta_q8 = -128;
            }
            lag_q8 += delta_q8;
        }
    }

    estimate.lag = best_lag;
    estimate.lag_q8 = lag_q8;
    estimate.peak_score = best_score;
    estimate.confidence = confidence > 255 ? 255 : (uint8_t)confidence;
    estimate.valid = 1;
    return estimate;
}

static size_t append_four_mic_history(const int32_t *east,
                                      const int32_t *west,
                                      const int32_t *north,
                                      const int32_t *south,
                                      size_t frame_count)
{
    const int32_t *input[4] = {east, west, north, south};
    if (frame_count >= DOA_ESTIMATOR_HISTORY_SAMPLES) {
        for (size_t mic = 0; mic < 4; mic++) {
            memcpy(four_mic_history[mic],
                   &input[mic][frame_count - DOA_ESTIMATOR_HISTORY_SAMPLES],
                   sizeof(four_mic_history[mic]));
        }
        four_mic_history_count = DOA_ESTIMATOR_HISTORY_SAMPLES;
        return four_mic_history_count;
    }

    size_t keep = four_mic_history_count;
    size_t max_keep = DOA_ESTIMATOR_HISTORY_SAMPLES - frame_count;
    if (keep > max_keep) {
        keep = max_keep;
    }
    size_t history_start = four_mic_history_count - keep;

    for (size_t mic = 0; mic < 4; mic++) {
        if (keep > 0 && history_start > 0) {
            memmove(four_mic_history[mic],
                    &four_mic_history[mic][history_start],
                    keep * sizeof(int32_t));
        }
        memcpy(&four_mic_history[mic][keep],
               input[mic],
               frame_count * sizeof(int32_t));
    }

    four_mic_history_count = keep + frame_count;
    return four_mic_history_count;
}

static int16_t q8_delay_to_samples(int32_t delay_q8)
{
    if (delay_q8 >= 0) {
        return (int16_t)((delay_q8 + (DOA_ESTIMATOR_LAG_Q8_SCALE / 2)) /
                         DOA_ESTIMATOR_LAG_Q8_SCALE);
    }
    return (int16_t)-((-delay_q8 + (DOA_ESTIMATOR_LAG_Q8_SCALE / 2)) /
                      DOA_ESTIMATOR_LAG_Q8_SCALE);
}

static uint8_t direction_confidence(const pair_estimate_t *ew,
                                    const pair_estimate_t *ns)
{
    uint32_t ew_weight = ew->valid ? (uint32_t)abs_int(ew->lag_q8) : 0;
    uint32_t ns_weight = ns->valid ? (uint32_t)abs_int(ns->lag_q8) : 0;
    uint32_t total_weight = ew_weight + ns_weight;

    if (total_weight == 0) {
        return 0;
    }

    uint32_t weighted =
        ((uint32_t)ew->confidence * ew_weight) +
        ((uint32_t)ns->confidence * ns_weight);
    return (uint8_t)(weighted / total_weight);
}

static void store_latest_state(const doa_estimator_state_t *next)
{
    latest_state.sample_delay = next->sample_delay;
    latest_state.vertical_delay = next->vertical_delay;
    latest_state.angle_index = next->angle_index;
    latest_state.confidence = next->confidence;
    latest_state.flags = next->flags;
    latest_state.energy = next->energy;
    latest_state.frame_counter = next->frame_counter;
    memcpy((void *)latest_state.mic_energy,
           next->mic_energy,
           sizeof(latest_state.mic_energy));
}

void doa_estimator_process_frame(const int32_t *mic0,
                                 const int32_t *mic1,
                                 size_t frame_count)
{
    doa_estimator_state_t next = {0};
    next.mic_energy[0] = mic_energy(mic0, frame_count);
    next.mic_energy[1] = mic_energy(mic1, frame_count);
    next.energy = clamp_u32((int64_t)next.mic_energy[0] + (int64_t)next.mic_energy[1]);
    next.frame_counter = latest_state.frame_counter + 1;
    next.angle_index = 12;

    pair_estimate_t estimate = estimate_pair(mic0, mic1, frame_count);

    if (next.energy >= DOA_ESTIMATOR_MIN_ENERGY && estimate.valid) {
        next.sample_delay = q8_delay_to_samples(estimate.lag_q8);
        next.confidence = estimate.confidence;
        next.flags = DOA_ESTIMATOR_FLAG_VALID;
    }

    store_latest_state(&next);
}

void doa_estimator_process_frame_4(const int32_t *east,
                                   const int32_t *west,
                                   const int32_t *north,
                                   const int32_t *south,
                                   size_t frame_count)
{
    doa_estimator_state_t next = {0};
    next.mic_energy[0] = mic_energy(east, frame_count);
    next.mic_energy[1] = mic_energy(west, frame_count);
    next.mic_energy[2] = mic_energy(north, frame_count);
    next.mic_energy[3] = mic_energy(south, frame_count);
    next.energy = clamp_u32((int64_t)next.mic_energy[0] +
                            (int64_t)next.mic_energy[1] +
                            (int64_t)next.mic_energy[2] +
                            (int64_t)next.mic_energy[3]);
    next.frame_counter = latest_state.frame_counter + 1;
    next.angle_index = 12;
    next.flags = DOA_ESTIMATOR_FLAG_FOUR_MIC;

    uint32_t frame_energy =
        mean_four_mic_energy(next.mic_energy, frame_count);
    if (!four_mic_signal_present(frame_energy)) {
        /*
         * Never mix low-level room noise into the correlation history. The
         * ESP renderer will hold the last valid direction briefly while this
         * invalid state is reported, then show its neutral listening glow.
         */
        four_mic_history_count = 0;
        if (invalid_direction_frames < UINT8_MAX) {
            invalid_direction_frames++;
        }
        if (invalid_direction_frames >= DOA_ESTIMATOR_INVALID_RESET_FRAMES) {
            smoothed_direction_valid = 0;
            smoothed_ew_delay_q8 = 0;
            smoothed_ns_delay_q8 = 0;
        }
        store_latest_state(&next);
        return;
    }

    size_t history_count = append_four_mic_history(
        east, west, north, south, frame_count);
    pair_estimate_t ew = estimate_pair(
        four_mic_history[0], four_mic_history[1], history_count);
    pair_estimate_t ns = estimate_pair(
        four_mic_history[2], four_mic_history[3], history_count);

    int ew_delay_q8 = ew.valid ? ew.lag_q8 : 0;
    int ns_delay_q8 = ns.valid ? ns.lag_q8 : 0;
    int vector_magnitude_q8 =
        abs_int(ew_delay_q8) + abs_int(ns_delay_q8);
    uint8_t confidence = direction_confidence(&ew, &ns);

    if (next.energy >= DOA_ESTIMATOR_MIN_ENERGY &&
        vector_magnitude_q8 >= DOA_ESTIMATOR_MIN_VECTOR_Q8 &&
        confidence >= DOA_ESTIMATOR_MIN_DIRECTION_CONFIDENCE) {
        if (!smoothed_direction_valid) {
            smoothed_ew_delay_q8 = ew_delay_q8;
            smoothed_ns_delay_q8 = ns_delay_q8;
            smoothed_direction_valid = 1;
        } else {
            smoothed_ew_delay_q8 +=
                ((ew_delay_q8 - smoothed_ew_delay_q8) *
                 DOA_ESTIMATOR_SMOOTH_NUMERATOR) /
                DOA_ESTIMATOR_SMOOTH_DENOMINATOR;
            smoothed_ns_delay_q8 +=
                ((ns_delay_q8 - smoothed_ns_delay_q8) *
                 DOA_ESTIMATOR_SMOOTH_NUMERATOR) /
                DOA_ESTIMATOR_SMOOTH_DENOMINATOR;
        }

        invalid_direction_frames = 0;
        next.sample_delay = q8_delay_to_samples(smoothed_ew_delay_q8);
        next.vertical_delay = q8_delay_to_samples(smoothed_ns_delay_q8);
        next.angle_index = estimate_angle_index(
            smoothed_ew_delay_q8, smoothed_ns_delay_q8);
        next.confidence = confidence;
        next.flags |= DOA_ESTIMATOR_FLAG_VALID;
    } else {
        if (invalid_direction_frames < UINT8_MAX) {
            invalid_direction_frames++;
        }
        if (invalid_direction_frames >= DOA_ESTIMATOR_INVALID_RESET_FRAMES) {
            smoothed_direction_valid = 0;
            smoothed_ew_delay_q8 = 0;
            smoothed_ns_delay_q8 = 0;
        }
    }

    store_latest_state(&next);
}

void doa_estimator_get_state(doa_estimator_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->sample_delay = latest_state.sample_delay;
    state->vertical_delay = latest_state.vertical_delay;
    state->angle_index = latest_state.angle_index;
    state->confidence = latest_state.confidence;
    state->flags = latest_state.flags;
    state->energy = latest_state.energy;
    state->frame_counter = latest_state.frame_counter;
    memcpy(state->mic_energy, (const void *)latest_state.mic_energy, sizeof(state->mic_energy));
}

void doa_estimator_reset(void)
{
    memset((void *)&latest_state, 0, sizeof(latest_state));
    memset(four_mic_history, 0, sizeof(four_mic_history));
    four_mic_history_count = 0;
    smoothed_ew_delay_q8 = 0;
    smoothed_ns_delay_q8 = 0;
    smoothed_direction_valid = 0;
    invalid_direction_frames = 0;
    noise_floor_energy = 0;
    noise_calibration_frames = 0;
    signal_energy_active = 0;
}
