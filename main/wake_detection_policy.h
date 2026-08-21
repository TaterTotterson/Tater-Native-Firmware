#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TATER_WAKE_ENVIRONMENT_FAR_FIELD = 0,
    TATER_WAKE_ENVIRONMENT_BALANCED,
    TATER_WAKE_ENVIRONMENT_STRICT,
    TATER_WAKE_ENVIRONMENT_TV_NEARBY,
} tater_wake_environment_t;

typedef enum {
    TATER_WAKE_SENSITIVITY_LOW = 0,
    TATER_WAKE_SENSITIVITY_NORMAL,
    TATER_WAKE_SENSITIVITY_HIGH,
    TATER_WAKE_SENSITIVITY_VERY_HIGH,
} tater_wake_sensitivity_t;

typedef struct {
    tater_wake_environment_t environment;
    tater_wake_sensitivity_t sensitivity;
    float probability_threshold;
    float peak_threshold;
    float minimum_rise_score;
    uint8_t min_active_windows;
    uint16_t cooldown_ms;
    bool require_verification;
} tater_wake_detection_policy_t;

tater_wake_detection_policy_t tater_wake_detection_policy_make(
    const char *sensitivity,
    const char *environment,
    float configured_threshold,
    uint8_t window
);
const char *tater_wake_environment_name(tater_wake_environment_t environment);
