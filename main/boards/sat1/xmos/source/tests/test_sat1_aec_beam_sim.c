#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xcore/hwtimer.h>
#include "aec.h"
#include "aec_schedule_4y.h"
#include "beamforming/sat1_beamformer.h"

static struct {
    uint64_t before;
    aec_state_t state;
    uint64_t after;
} guarded;
static sat1_beamformer_t beam;
static int32_t raw[4][240], reference[1][240], residual[4][240], output[240];
#define CANARY UINT64_C(0x5d3a9f2647bc180e)

int main(void)
{
    unsigned max_ticks = 0;
    unsigned fractional_frames = 0;
    for (unsigned boot = 0; boot < 3; ++boot) {
        memset(&guarded, 0, sizeof(guarded));
        guarded.before = guarded.after = CANARY;
        doa_estimator_reset();
        sat1_beamformer_reset(&beam);
        aec_init(&guarded.state, 4, 1, 10, 5, &aec_tdist_chans4_threads1);
        assert(guarded.state.shared_state.num_y_channels == 4);
        for (unsigned frame = 0; frame < 16; ++frame) {
            for (unsigned i = 0; i < 240; ++i) {
                unsigned n = frame * 240 + i;
                reference[0][i] = frame < 4 ? 0 : (int32_t)((n * 7919u) % 100003u) * 1024;
                for (unsigned mic = 0; mic < 4; ++mic)
                    raw[mic][i] = (int32_t)(((n + mic * 2u) * 3571u) % 100019u) * 512;
            }
            const int32_t *raw_mics[4] = {raw[0], raw[1], raw[2], raw[3]};
            const int32_t *clean_mics[4] = {residual[0], residual[1], residual[2], residual[3]};
            unsigned start = get_reference_time();
            sat1_beamformer_analyze_frame(&beam, raw_mics, 240);
            doa_estimator_set_mic_health(beam.active_mic_mask);
            doa_estimator_set_control(frame >= 4 && frame < 12 ? DOA_ESTIMATOR_CONTROL_FORCE_OMNI : 0);
            doa_estimator_process_frame_4(raw[0], raw[1], raw[2], raw[3], 240);
            aec_process_frame(&guarded.state, residual, NULL, raw, reference);
            doa_estimator_state_t direction;
            doa_estimator_get_state(&direction);
            /* Cover the expensive steered path even when synthetic raw audio
             * does not give the estimator a confident fractional direction.
             * Include half/quarter samples and a moving-talker transition.
             */
            if (frame >= 12) {
                static const int16_t delays[4][2] = {
                    {256, 256}, {128, 128}, {-891, 715}, {715, -891}
                };
                direction.flags |= DOA_ESTIMATOR_FLAG_VALID | DOA_ESTIMATOR_FLAG_FOUR_MIC;
                direction.sample_delay_q8 = delays[frame - 12][0];
                direction.vertical_delay_q8 = delays[frame - 12][1];
                ++fractional_frames;
            }
            unsigned calibrated_frames = beam.calibration_frames;
            sat1_beamformer_process_calibrated_frame(&beam, output, clean_mics, 240, &direction);
            assert(beam.calibration_frames == calibrated_frames);
            if (frame >= 4) {
                bool changing = false;
                for (unsigned i = 1; i < 240; ++i)
                    changing |= output[i] != output[0];
                assert(changing);
            }
            unsigned ticks = get_reference_time() - start;
            if (ticks > max_ticks) max_ticks = ticks;
            assert(ticks < 1200000); /* 12 ms, single-thread simulator only. */
            assert(guarded.before == CANARY && guarded.after == CANARY);
            assert(direction.frame_counter > 0 && beam.active_mic_mask != 0);
        }
        aec_reset_state(&guarded.state);
        assert(guarded.before == CANARY && guarded.after == CANARY);
    }
    assert(fractional_frames == 12);
    printf("Sat1 real 4Y1X AEC + raw DoA/calibration + residual beam: 48 frames, %u fractional/transition frames and 3 initializations passed; max simulator reference ticks=%u\n",
           fractional_frames, max_ticks);
    return 0;
}
