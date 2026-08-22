#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAT1_BETA_REQUIRED_PD_MV 9000U

typedef struct {
    uint8_t object_position;
    uint16_t voltage_mv;
    uint16_t current_ma;
} sat1_pd_fixed_selection_t;

bool sat1_pd_select_beta_9v_pdo(
    const uint32_t *objects,
    size_t object_count,
    sat1_pd_fixed_selection_t *selection
);
