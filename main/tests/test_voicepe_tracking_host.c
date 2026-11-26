#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "doa/doa_estimator.h"
#include "boards/voice_pe/voicepe_audio_hints.h"

static int32_t mic0[240], mic1[240];

static doa_estimator_state_t candidate_for_lag(int lag)
{
    int32_t signal[248];
    uint32_t rng = 9;
    for (unsigned i = 0; i < 248; ++i) {
        rng = rng * 1664525u + 1013904223u;
        signal[i] = ((int32_t)(rng >> 16) - 32768) * 8192;
    }
    for (unsigned i = 0; i < 240; ++i) {
        mic0[i] = signal[i + 4];
        mic1[i] = signal[(int)i + 4 - lag];
    }
    doa_estimator_state_t candidate;
    doa_estimator_analyze_frame(mic0, mic1, 240, &candidate);
    assert(candidate.flags & VOICEPE_DOA_FLAG_VALID);
    assert(candidate.sample_delay == lag);
    assert(candidate.confidence > 100);
    return candidate;
}

static doa_estimator_state_t latest(void)
{
    doa_estimator_state_t state;
    doa_estimator_get_state(&state);
    return state;
}

static void test_directions_and_speech_gate(void)
{
    for (int lag = -4; lag <= 4; ++lag) {
        doa_estimator_reset();
        doa_estimator_state_t candidate = candidate_for_lag(lag);
        doa_estimator_publish_frame(&candidate, false, false, false, 0);
        assert(!(latest().flags & VOICEPE_DOA_FLAG_VALID));
        doa_estimator_publish_frame(&candidate, true, false, false, 15);
        assert(latest().flags & VOICEPE_DOA_FLAG_VALID);
        assert(latest().sample_delay == lag);
        assert(latest().frame_counter == 2);
        assert(candidate.frame_counter == 0);
        for (int i = 0; i < 6; ++i)
            doa_estimator_publish_frame(&candidate, false, false, false, 30 + i * 15);
        assert(!(latest().flags & VOICEPE_DOA_FLAG_VALID));
    }
}

static void test_playback_and_hints(void)
{
    doa_estimator_reset();
    doa_estimator_state_t candidate = candidate_for_lag(2);
    /* Near speech over playback remains directional without any host. */
    doa_estimator_publish_frame(&candidate, true, true, false, 0);
    assert(latest().flags & VOICEPE_DOA_FLAG_VALID);
    /* Echo evidence suppresses a music-only direction. */
    doa_estimator_publish_frame(&candidate, true, true, true, 15);
    assert(!(latest().flags & VOICEPE_DOA_FLAG_VALID));
    doa_estimator_set_host_hints(VOICEPE_DOA_HINT_PLAYBACK | VOICEPE_DOA_HINT_LISTENING, 20);
    doa_estimator_publish_frame(&candidate, true, true, true, 21);
    assert(latest().flags & VOICEPE_DOA_FLAG_VALID);
    assert(latest().flags & VOICEPE_DOA_FLAG_LISTENING);
    /* The hint cannot manufacture speech or a valid correlation. */
    doa_estimator_reset();
    doa_estimator_set_host_hints(VOICEPE_DOA_HINT_LISTENING, 0);
    doa_estimator_publish_frame(&candidate, false, false, false, 1);
    assert(!(latest().flags & VOICEPE_DOA_FLAG_VALID));
    memset(mic0, 0, sizeof(mic0)); memset(mic1, 0, sizeof(mic1));
    doa_estimator_analyze_frame(mic0, mic1, 240, &candidate);
    doa_estimator_publish_frame(&candidate, true, false, false, 2);
    assert(!(latest().flags & VOICEPE_DOA_FLAG_VALID));
}

static void test_expiry_refresh_and_clock_wrap(void)
{
    doa_estimator_reset();
    doa_estimator_state_t candidate = candidate_for_lag(-2);
    doa_estimator_set_host_hints(VOICEPE_DOA_HINT_LISTENING, 100);
    doa_estimator_publish_frame(&candidate, true, true, true, 849);
    assert(latest().flags & VOICEPE_DOA_FLAG_VALID);
    doa_estimator_publish_frame(&candidate, true, true, true, 850);
    assert(!(latest().flags & VOICEPE_DOA_FLAG_VALID));
    assert(!(latest().flags & VOICEPE_DOA_FLAG_HINTS_FRESH));
    /* An expired host does not suppress locally detected speech. */
    doa_estimator_publish_frame(&candidate, true, true, false, 851);
    assert(latest().flags & VOICEPE_DOA_FLAG_VALID);
    doa_estimator_set_host_hints(VOICEPE_DOA_HINT_LISTENING, 900);
    doa_estimator_set_host_hints(VOICEPE_DOA_HINT_LISTENING, 1500);
    doa_estimator_publish_frame(&candidate, true, true, true, 1800);
    assert(latest().flags & VOICEPE_DOA_FLAG_LISTENING);
    doa_estimator_set_host_hints(0, 1801);
    doa_estimator_publish_frame(&candidate, true, true, true, 1802);
    assert(!(latest().flags & VOICEPE_DOA_FLAG_LISTENING));
    assert(!(latest().flags & VOICEPE_DOA_FLAG_VALID));
    doa_estimator_set_host_hints(VOICEPE_DOA_HINT_LISTENING, UINT32_MAX - 200);
    doa_estimator_publish_frame(&candidate, true, true, true, 400);
    assert(latest().flags & VOICEPE_DOA_FLAG_VALID);
    doa_estimator_publish_frame(&candidate, true, true, true, 600);
    assert(!(latest().flags & VOICEPE_DOA_FLAG_HINTS_FRESH));
}

static void test_host_capability_and_refresh(void)
{
    voicepe_audio_hints_t host;
    uint8_t caps[4] = {'V', 'P', 1, 3};
    voicepe_audio_hints_init(&host, NULL);
    assert(!voicepe_audio_hints_due(&host, 3, 1000));
    for (int field = 0; field < 4; ++field) {
        uint8_t bad[4]; memcpy(bad, caps, sizeof(bad)); bad[field] = 0;
        voicepe_audio_hints_init(&host, bad);
        assert(!voicepe_audio_hints_due(&host, 3, 1000));
    }
    voicepe_audio_hints_init(&host, caps);
    assert(voicepe_audio_hints_due(&host, 0, 0));
    /* Failure leaves the write due; only a successful transfer acknowledges it. */
    assert(voicepe_audio_hints_due(&host, 0, 1));
    voicepe_audio_hints_sent(&host, 0, 1);
    assert(!voicepe_audio_hints_due(&host, 0, 250));
    assert(voicepe_audio_hints_due(&host, 0, 251));
    assert(voicepe_audio_hints_due(&host, 1, 2));
    voicepe_audio_hints_sent(&host, 3, UINT32_MAX - 100);
    assert(!voicepe_audio_hints_due(&host, 3, 100));
    assert(voicepe_audio_hints_due(&host, 3, 200));
}

static void test_full_scale_and_legacy_pipeline(void)
{
    doa_estimator_reset();
    for (unsigned i = 0; i < 240; ++i) {
        mic0[i] = (i & 1) ? INT32_MAX : INT32_MIN;
        mic1[i] = mic0[i];
    }
    doa_estimator_state_t candidate;
    doa_estimator_analyze_frame(mic0, mic1, 240, &candidate);
    assert(candidate.energy == UINT32_MAX);
    (void)candidate_for_lag(3);
    doa_estimator_process_frame(mic0, mic1, 240);
    assert(latest().sample_delay == 3);
    assert(latest().flags & VOICEPE_DOA_FLAG_VALID);
    doa_estimator_analyze_frame(NULL, NULL, 0, &candidate);
    assert(candidate.flags == 0);
    doa_estimator_reset();
    assert(latest().frame_counter == 0 && latest().flags == 0);
}

static void test_tracking_never_changes_audio(void)
{
    doa_estimator_reset();
    int32_t before0[240], before1[240];
    (void)candidate_for_lag(-1);
    memcpy(before0, mic0, sizeof(mic0)); memcpy(before1, mic1, sizeof(mic1));
    for (unsigned i = 0; i < 500; ++i) {
        doa_estimator_state_t candidate;
        doa_estimator_analyze_frame(mic0, mic1, 240, &candidate);
        doa_estimator_set_host_hints(i & 3, i * 15);
        doa_estimator_publish_frame(&candidate, (i & 1) != 0, (i & 2) != 0,
                                    (i & 4) != 0, i * 15);
    }
    assert(memcmp(before0, mic0, sizeof(mic0)) == 0);
    assert(memcmp(before1, mic1, sizeof(mic1)) == 0);
    assert(latest().frame_counter == 500);
}

static void test_no_ema_onset_delay(void)
{
    const uint32_t one = 1u << 30;
    const uint32_t alpha = 1041529569; /* The SDK's .97 EMA. */
    uint16_t quiet = 328, speech = 29491; /* .01 -> .90 on the first frame. */
    uint16_t smoothed = ((uint64_t)quiet * alpha +
        (uint64_t)speech * (one - alpha) + one / 2) / one;
    assert(smoothed < 16384); /* The old display gate is still closed. */
    uint16_t instant = doa_estimator_instant_vnr(quiet, smoothed, alpha);
    assert(instant > speech - 40 && instant < speech + 40);
    doa_estimator_reset();
    doa_estimator_state_t candidate = candidate_for_lag(1);
    doa_estimator_publish_frame(&candidate, instant >= 16384, true, false, 0);
    assert(latest().flags & VOICEPE_DOA_FLAG_VALID);
    assert(latest().frame_counter == 1);
    assert(doa_estimator_instant_vnr(0, 0, alpha) == 0);
    assert(doa_estimator_instant_vnr(0, 32768, alpha) == 32768);
    assert(doa_estimator_instant_vnr(32768, 0, alpha) == 0);
    assert(doa_estimator_instant_vnr(200, 900, 0) == 900);
    assert(doa_estimator_instant_vnr(200, 900, one) == 900);
}

static void test_broad_speech_peak_and_dc(void)
{
    int32_t filtered[248];
    int64_t filter = 0;
    uint32_t rng = 77;
    for (unsigned i = 0; i < 248; ++i) {
        rng = rng * 1664525u + 1013904223u;
        filter = (filter * 31 + ((int32_t)(rng >> 16) - 32768) * 8192) / 32;
        filtered[i] = filter;
    }
    for (unsigned i = 0; i < 240; ++i) {
        mic0[i] = filtered[i + 4] + 100000000;
        mic1[i] = filtered[i + 2] * 2 - 100000000;
    }
    doa_estimator_state_t candidate;
    doa_estimator_analyze_frame(mic0, mic1, 240, &candidate);
    assert(candidate.sample_delay == 2 && candidate.confidence >= 240);
    /* Independent microphone noise is not a credible direction. */
    for (unsigned i = 0; i < 240; ++i) {
        rng = rng * 1664525u + 1013904223u;
        mic0[i] = ((int32_t)(rng >> 16) - 32768) * 8192;
        rng = rng * 1664525u + 1013904223u;
        mic1[i] = ((int32_t)(rng >> 16) - 32768) * 8192;
    }
    doa_estimator_analyze_frame(mic0, mic1, 240, &candidate);
    assert(!(candidate.flags & VOICEPE_DOA_FLAG_VALID));
    for (unsigned i = 0; i < 240; ++i) mic0[i] = mic1[i] = 100000000;
    doa_estimator_analyze_frame(mic0, mic1, 240, &candidate);
    assert(!(candidate.flags & VOICEPE_DOA_FLAG_VALID));
}

int main(void)
{
    test_directions_and_speech_gate();
    test_playback_and_hints();
    test_expiry_refresh_and_clock_wrap();
    test_host_capability_and_refresh();
    test_full_scale_and_legacy_pipeline();
    test_tracking_never_changes_audio();
    test_no_ema_onset_delay();
    test_broad_speech_peak_and_dc();
    puts("Voice PE tracking: direction, speech/echo gating, hint expiry, capability and legacy checks passed");
    return 0;
}
