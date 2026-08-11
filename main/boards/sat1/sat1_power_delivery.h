#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef esp_err_t (*sat1_pd_i2c_read_fn)(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);
typedef esp_err_t (*sat1_pd_i2c_write_fn)(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);

typedef enum {
    SAT1_PD_STATE_UNINITIALIZED = 0,
    SAT1_PD_STATE_NEGOTIATING,
    SAT1_PD_STATE_EXPLICIT_CONTRACT,
    SAT1_PD_STATE_FALLBACK_5V,
    SAT1_PD_STATE_UNAVAILABLE,
    SAT1_PD_STATE_ERROR,
} sat1_pd_state_t;

typedef struct {
    sat1_pd_state_t state;
    bool controller_present;
    bool attached;
    bool explicit_contract;
    bool negotiation_failed;
    uint8_t device_id;
    uint8_t cc_pin;
    uint8_t source_pdo_count;
    uint16_t requested_voltage_mv;
    uint16_t contract_voltage_mv;
    uint16_t contract_current_ma;
} sat1_pd_status_t;

esp_err_t sat1_pd_init(sat1_pd_i2c_read_fn read_fn, sat1_pd_i2c_write_fn write_fn);
bool sat1_pd_wait_ready(uint32_t timeout_ms);
bool sat1_pd_status_snapshot(sat1_pd_status_t *out);
uint8_t sat1_pd_recommended_tas_mode(void);
const char *sat1_pd_state_name(sat1_pd_state_t state);
