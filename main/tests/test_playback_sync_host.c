#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "playback_sync.h"

static void test_identity_copy(void)
{
    const int16_t input[] = {10, -10, 20, -20, 30, -30};
    int16_t output[6] = {0};
    tater_playback_sync_resample_stereo(input, 3, output, 3);
    for (size_t index = 0; index < 6; index++) {
        assert(output[index] == input[index]);
    }
}

static void test_slew_keeps_endpoints(void)
{
    const int16_t input[] = {0, 0, 100, -100, 200, -200, 300, -300, 400, -400};
    int16_t output[8] = {0};
    tater_playback_sync_resample_stereo(input, 5, output, 4);
    assert(output[0] == 0);
    assert(output[1] == 0);
    assert(output[6] == 400);
    assert(output[7] == -400);
    assert(output[2] > 100 && output[2] < 200);
}

static void test_fade_finishes(void)
{
    int16_t samples[] = {1000, -1000, 1000, -1000, 1000, -1000, 1000, -1000};
    uint32_t remaining = 4;
    tater_playback_sync_fade_in(samples, 4, &remaining, 4);
    assert(remaining == 0);
    assert(samples[0] > 0 && samples[0] < 1000);
    assert(samples[6] == 1000);
    assert(samples[7] == -1000);
}

static void test_slew_is_distributed_over_settle_window(void)
{
    tater_playback_sync_slew_t slew;
    tater_playback_sync_slew_init(&slew);
    tater_playback_sync_slew_queue(&slew, 48, 48000, 480);
    int corrections = 0;
    for (int chunk = 0; chunk < 188; chunk++) {
        corrections += tater_playback_sync_slew_next_step(&slew, 256);
        if (chunk < 2) {
            assert(corrections == 0);
        }
    }
    assert(corrections == 48);
    assert(slew.pending_frames == 0);
}

static void test_slew_step_can_be_restored_after_starvation(void)
{
    tater_playback_sync_slew_t slew;
    tater_playback_sync_slew_init(&slew);
    tater_playback_sync_slew_queue(&slew, -1, 256, 480);
    int32_t step = tater_playback_sync_slew_next_step(&slew, 256);
    assert(step == -1);
    tater_playback_sync_slew_restore_step(&slew, step);
    assert(slew.pending_frames == -1);
}

int main(void)
{
    test_identity_copy();
    test_slew_keeps_endpoints();
    test_fade_finishes();
    test_slew_is_distributed_over_settle_window();
    test_slew_step_can_be_restored_after_starvation();
    puts("playback_sync host tests passed");
    return 0;
}
