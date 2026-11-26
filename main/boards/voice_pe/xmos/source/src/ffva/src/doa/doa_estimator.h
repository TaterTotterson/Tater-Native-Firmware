#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define DOA_ESTIMATOR_MAX_LAG_SAMPLES (4)
#define DOA_ESTIMATOR_FLAG_VALID      (1u << 0)

typedef struct {
    /* Positive values mean mic1 best matches a later sample than mic0. */
    int16_t sample_delay;
    uint8_t confidence;
    uint8_t flags;
    uint32_t energy;
    uint32_t frame_counter;
} doa_estimator_state_t;

void doa_estimator_process_frame(const int32_t *mic0,
                                 const int32_t *mic1,
                                 size_t frame_count);

void doa_estimator_get_state(doa_estimator_state_t *state);

/* Analyze the two AEC outputs before IC changes either input. Publish only
 * after the same frame's VNR evidence is available. Neither operation changes
 * microphone samples or the audio pipeline's adaptation/output. */
void doa_estimator_analyze_frame(const int32_t *mic0, const int32_t *mic1,
                                 size_t frame_count, doa_estimator_state_t *state);
void doa_estimator_publish_frame(const doa_estimator_state_t *candidate,
                                 bool speech_present, bool reference_active,
                                 bool echo_dominated, uint32_t now_ms);
void doa_estimator_set_host_hints(uint8_t flags, uint32_t now_ms);
void doa_estimator_reset(void);

/* Recover the current inference from an SDK EMA without changing that EMA,
 * running another model, or delaying any samples. Probabilities are Q15. */
uint16_t doa_estimator_instant_vnr(uint16_t before_q15, uint16_t after_q15,
                                  uint32_t alpha_q30);
