#include "wake_detection_policy.h"

#include <stddef.h>
#include <string.h>

#define WAKE_SCORE_STEP (1.0f / 255.0f)

static float clamp_threshold(float threshold)
{
    if (threshold < 0.01f) {
        return 0.01f;
    }
    if (threshold > 0.99f) {
        return 0.99f;
    }
    return threshold;
}
static float min_float(float left, float right)
{
    return left < right ? left : right;
}

static float max_float(float left, float right)
{
    return left > right ? left : right;
}

static tater_wake_environment_t parse_environment(const char *value)
{
    if (!value || !value[0]) {
        return TATER_WAKE_ENVIRONMENT_BALANCED;
    }
    if (strcmp(value, "far_field") == 0 || strcmp(value, "quiet") == 0
        || strcmp(value, "quiet_room") == 0 || strcmp(value, "very_sensitive") == 0) {
        return TATER_WAKE_ENVIRONMENT_FAR_FIELD;
    }
    if (strcmp(value, "strict") == 0) {
        return TATER_WAKE_ENVIRONMENT_STRICT;
    }
    if (strcmp(value, "tv_nearby") == 0 || strcmp(value, "tv") == 0
        || strcmp(value, "near_tv") == 0) {
        return TATER_WAKE_ENVIRONMENT_TV_NEARBY;
    }
    return TATER_WAKE_ENVIRONMENT_BALANCED;
}

static tater_wake_sensitivity_t parse_sensitivity(const char *value)
{
    if (!value || !value[0] || strcmp(value, "normal") == 0 || strcmp(value, "medium") == 0) {
        return TATER_WAKE_SENSITIVITY_NORMAL;
    }
    if (strcmp(value, "low") == 0 || strcmp(value, "conservative") == 0) {
        return TATER_WAKE_SENSITIVITY_LOW;
    }
    if (strcmp(value, "very_high") == 0 || strcmp(value, "very_sensitive") == 0) {
        return TATER_WAKE_SENSITIVITY_VERY_HIGH;
    }
    if (strcmp(value, "high") == 0 || strcmp(value, "sensitive") == 0) {
        return TATER_WAKE_SENSITIVITY_HIGH;
    }
    return TATER_WAKE_SENSITIVITY_NORMAL;
}

static float threshold_for_sensitivity(
    tater_wake_sensitivity_t sensitivity,
    float configured_threshold
)
{
    switch (sensitivity) {
        case TATER_WAKE_SENSITIVITY_LOW:
            return max_float(configured_threshold, 251.0f * WAKE_SCORE_STEP);
        case TATER_WAKE_SENSITIVITY_HIGH:
            return min_float(configured_threshold, 242.0f * WAKE_SCORE_STEP);
        case TATER_WAKE_SENSITIVITY_VERY_HIGH:
            return min_float(configured_threshold, 235.0f * WAKE_SCORE_STEP);
        case TATER_WAKE_SENSITIVITY_NORMAL:
        default:
            return configured_threshold;
    }
}

static float tv_candidate_ceiling(tater_wake_sensitivity_t sensitivity)
{
    switch (sensitivity) {
        case TATER_WAKE_SENSITIVITY_LOW:
            return 235.0f * WAKE_SCORE_STEP;
        case TATER_WAKE_SENSITIVITY_HIGH:
            return 219.0f * WAKE_SCORE_STEP;
        case TATER_WAKE_SENSITIVITY_VERY_HIGH:
            return 209.0f * WAKE_SCORE_STEP;
        case TATER_WAKE_SENSITIVITY_NORMAL:
        default:
            return 224.0f * WAKE_SCORE_STEP;
    }
}

static uint8_t clamp_window(uint8_t window)
{
    return window == 0 ? 1 : window;
}

tater_wake_detection_policy_t tater_wake_detection_policy_make(
    const char *sensitivity,
    const char *environment,
    float configured_threshold,
    uint8_t window
)
{
    tater_wake_detection_policy_t policy = {0};
    policy.environment = parse_environment(environment);
    policy.sensitivity = parse_sensitivity(sensitivity);
    window = clamp_window(window);

    float threshold = threshold_for_sensitivity(
        policy.sensitivity,
        clamp_threshold(configured_threshold)
    );
    policy.peak_threshold = threshold;
    policy.minimum_rise_score = -1.0f;
    policy.min_active_windows = (uint8_t)((window + 1U) / 2U);
    policy.cooldown_ms = 1200;

    switch (policy.environment) {
        case TATER_WAKE_ENVIRONMENT_FAR_FIELD:
            policy.min_active_windows = 1;
            policy.cooldown_ms = 800;
            break;
        case TATER_WAKE_ENVIRONMENT_STRICT:
            threshold = max_float(threshold, 247.0f * WAKE_SCORE_STEP);
            policy.peak_threshold = threshold;
            policy.minimum_rise_score = -8.0f * WAKE_SCORE_STEP;
            policy.min_active_windows = (uint8_t)(((window * 2U) + 2U) / 3U);
            if (policy.min_active_windows < 2 && window >= 2) {
                policy.min_active_windows = 2;
            }
            policy.cooldown_ms = 1600;
            break;
        case TATER_WAKE_ENVIRONMENT_TV_NEARBY:
            threshold = min_float(threshold, tv_candidate_ceiling(policy.sensitivity));
            policy.peak_threshold = max_float(threshold, 235.0f * WAKE_SCORE_STEP);
            policy.minimum_rise_score = -8.0f * WAKE_SCORE_STEP;
            policy.min_active_windows = (uint8_t)((window + 1U) / 2U);
            policy.cooldown_ms = 2400;
            policy.require_verification = true;
            break;
        case TATER_WAKE_ENVIRONMENT_BALANCED:
        default:
            break;
    }

    if (policy.min_active_windows > window) {
        policy.min_active_windows = window;
    }
    policy.probability_threshold = clamp_threshold(threshold);
    return policy;
}

const char *tater_wake_environment_name(tater_wake_environment_t environment)
{
    switch (environment) {
        case TATER_WAKE_ENVIRONMENT_FAR_FIELD:
            return "far_field";
        case TATER_WAKE_ENVIRONMENT_STRICT:
            return "strict";
        case TATER_WAKE_ENVIRONMENT_TV_NEARBY:
            return "tv_nearby";
        case TATER_WAKE_ENVIRONMENT_BALANCED:
        default:
            return "balanced";
    }
}
