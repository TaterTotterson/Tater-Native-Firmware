#pragma once

#include "app_conf.h"

#if appconfSAT1_AUDIO_BENCH
#include "audio_bench_format.h"

typedef enum {
    SAT1_BENCH_CAPTURE,
    SAT1_BENCH_REFERENCE_WAIT,
    SAT1_BENCH_MIC_WAIT,
    SAT1_BENCH_RAW_ANALYSIS,
    SAT1_BENCH_AEC,
    SAT1_BENCH_INTERTILE_TX,
    SAT1_BENCH_INTERTILE_RX,
    SAT1_BENCH_IC,
    SAT1_BENCH_NS,
    SAT1_BENCH_AGC,
    SAT1_BENCH_I2S_OUTPUT,
    SAT1_BENCH_USB_OUTPUT,
    SAT1_BENCH_SPEAKER_INPUT,
    SAT1_BENCH_REFERENCE_TX,
    SAT1_BENCH_FRAME_POOL_WAIT,
    SAT1_BENCH_STAGE_COUNT
} sat1_bench_stage_t;

void sat1_bench_begin(sat1_bench_stage_t stage);
void sat1_bench_end(sat1_bench_stage_t stage);
void sat1_bench_monitor_start(void);
void sat1_bench_pool_init(void);
void *sat1_bench_frame_acquire(size_t size);
void sat1_bench_frame_release(void *frame);
void sat1_bench_arm_capture(void);
void sat1_bench_note_raw(const int32_t *samples, size_t count);
void sat1_bench_snapshot(sat1_bench_frame_t *frame,
                         const int32_t *left, const int32_t *right);
void sat1_bench_usb_drop(void);
#endif
