#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "wake_detection_policy.h"

static void assert_close(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.0001f);
}
static void test_sensitivity_changes_threshold(void)
{
    tater_wake_detection_policy_t normal = tater_wake_detection_policy_make(
        "normal", "far_field", 0.99f, 5);
    tater_wake_detection_policy_t high = tater_wake_detection_policy_make(
        "high", "far_field", 0.99f, 5);
    tater_wake_detection_policy_t very_high = tater_wake_detection_policy_make(
        "very_high", "far_field", 0.99f, 5);
    tater_wake_detection_policy_t low = tater_wake_detection_policy_make(
        "low", "far_field", 0.90f, 5);

    assert_close(normal.probability_threshold, 0.99f);
    assert_close(high.probability_threshold, 242.0f / 255.0f);
    assert_close(very_high.probability_threshold, 235.0f / 255.0f);
    assert_close(low.probability_threshold, 251.0f / 255.0f);
    assert(high.probability_threshold < normal.probability_threshold);
}

static void test_tv_profile_is_sensitive_and_verified(void)
{
    tater_wake_detection_policy_t policy = tater_wake_detection_policy_make(
        "high", "tv_nearby", 0.99f, 5);

    assert(policy.environment == TATER_WAKE_ENVIRONMENT_TV_NEARBY);
    assert_close(policy.probability_threshold, 219.0f / 255.0f);
    assert_close(policy.peak_threshold, 235.0f / 255.0f);
    assert_close(policy.minimum_rise_score, -8.0f / 255.0f);
    assert(policy.min_active_windows == 3);
    assert(policy.cooldown_ms == 2400);
    assert(policy.require_verification);
    assert(policy.probability_threshold < policy.peak_threshold);
}

static void test_strict_profile_remains_conservative(void)
{
    tater_wake_detection_policy_t policy = tater_wake_detection_policy_make(
        "high", "strict", 0.90f, 5);

    assert_close(policy.probability_threshold, 247.0f / 255.0f);
    assert_close(policy.peak_threshold, policy.probability_threshold);
    assert(policy.min_active_windows == 4);
    assert(!policy.require_verification);
}

static void test_aliases_and_short_window(void)
{
    tater_wake_detection_policy_t policy = tater_wake_detection_policy_make(
        "sensitive", "near_tv", 1.5f, 0);

    assert(policy.environment == TATER_WAKE_ENVIRONMENT_TV_NEARBY);
    assert(policy.sensitivity == TATER_WAKE_SENSITIVITY_HIGH);
    assert(policy.min_active_windows == 1);
    assert(policy.require_verification);
    assert(policy.probability_threshold <= 0.99f);
    assert(tater_wake_environment_name(policy.environment)[0] == 't');
}

int main(void)
{
    test_sensitivity_changes_threshold();
    test_tv_profile_is_sensitive_and_verified();
    test_strict_profile_remains_conservative();
    test_aliases_and_short_window();
    puts("wake detection policy host tests passed");
    return 0;
}
