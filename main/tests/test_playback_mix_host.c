#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../playback_mix.h"

static void test_fixed_background_gain(void)
{
    tater_playback_mix_state_t state;
    const int16_t foreground[] = {1000, -1000, 2000, -2000};
    const int16_t background[] = {4000, 4000, -4000, -4000};
    int16_t output[4] = {0};

    tater_playback_mix_init(&state, 25);
    tater_playback_mix_frames(&state, foreground, background, output, 2);

    assert(output[0] == 1750);
    assert(output[1] == 250);
    assert(output[2] == 500);
    assert(output[3] == -2500);
}

static void test_overlay_mix_reserves_headroom(void)
{
    tater_playback_mix_state_t state;
    const int16_t foreground[] = {30000, -30000};
    const int16_t background[] = {10000, -10000};
    int16_t output[2] = {0};

    tater_playback_mix_init(&state, 20);
    tater_playback_mix_frames(&state, foreground, background, output, 1);

    assert(output[0] > 25000 && output[0] < 27000);
    assert(output[1] < -25000 && output[1] > -27000);
}

static void test_ramp_reaches_target(void)
{
    tater_playback_mix_state_t state;
    const int16_t background[] = {
        10000, 10000,
        10000, 10000,
        10000, 10000,
        10000, 10000,
    };
    int16_t output[8] = {0};

    tater_playback_mix_init(&state, 0);
    tater_playback_mix_set_background(&state, 40, 4);
    tater_playback_mix_frames(&state, NULL, background, output, 4);

    assert(output[0] > 0);
    assert(output[0] < output[2]);
    assert(output[2] < output[4]);
    assert(output[4] < output[6]);
    assert(output[6] >= 3999);
    assert(output[6] <= 4000);
    assert(state.ramp_frames_remaining == 0);
}

static void test_overlay_duck_and_release_preserves_background(void)
{
    tater_playback_mix_state_t state;
    const int16_t foreground[] = {
        1000, 1000,
        1000, 1000,
        1000, 1000,
        1000, 1000,
    };
    const int16_t background[] = {
        10000, 10000,
        10000, 10000,
        10000, 10000,
        10000, 10000,
    };
    int16_t ducked[8] = {0};
    int16_t released[8] = {0};

    tater_playback_mix_init(&state, 100);
    tater_playback_mix_set_background(&state, 20, 4);
    tater_playback_mix_frames(&state, foreground, background, ducked, 4);
    assert(ducked[0] > ducked[6]);
    assert(ducked[6] >= 2799);
    assert(ducked[6] <= 2800);

    tater_playback_mix_set_background(&state, 100, 4);
    tater_playback_mix_frames(&state, NULL, background, released, 4);
    assert(released[0] < released[6]);
    assert(released[6] >= 9999);
    assert(released[6] <= 10000);
    assert(state.ramp_frames_remaining == 0);
}

static void test_stereo_channel_routing(void)
{
    const int16_t source[] = {
        1200, -800,
        32767, -32768,
    };
    int16_t left[4];
    int16_t right[4];
    int16_t mono[4];
    int16_t stereo[4];

    for (size_t index = 0; index < 4; index++) {
        left[index] = source[index];
        right[index] = source[index];
        mono[index] = source[index];
        stereo[index] = source[index];
    }

    tater_playback_route_channel(TATER_PLAYBACK_CHANNEL_LEFT, left, 2);
    assert(left[0] == 1200 && left[1] == 1200);
    assert(left[2] == 32767 && left[3] == 32767);

    tater_playback_route_channel(TATER_PLAYBACK_CHANNEL_RIGHT, right, 2);
    assert(right[0] == -800 && right[1] == -800);
    assert(right[2] == -32768 && right[3] == -32768);

    tater_playback_route_channel(TATER_PLAYBACK_CHANNEL_MONO, mono, 2);
    assert(mono[0] == 200 && mono[1] == 200);
    assert(mono[2] == 0 && mono[3] == 0);

    tater_playback_route_channel(TATER_PLAYBACK_CHANNEL_STEREO, stereo, 2);
    for (size_t index = 0; index < 4; index++) {
        assert(stereo[index] == source[index]);
    }
}

int main(void)
{
    test_fixed_background_gain();
    test_overlay_mix_reserves_headroom();
    test_ramp_reaches_target();
    test_overlay_duck_and_release_preserves_background();
    test_stereo_channel_routing();
    puts("playback mixer host tests passed");
    return 0;
}
