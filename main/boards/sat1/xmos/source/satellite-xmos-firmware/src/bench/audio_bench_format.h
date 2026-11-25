#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Lab wire format: 16 kHz signed PCM32, with bit-exact metadata words. */
#define SAT1_BENCH_FRAME_SAMPLES 240
#define SAT1_BENCH_AUDIO_CHANNELS 8
#define SAT1_BENCH_CHANNELS 12
#define SAT1_BENCH_FORMAT_VERSION 1
#define SAT1_BENCH_RAW_VALID 1u
#define SAT1_BENCH_REFERENCE_VALID 2u
#define SAT1_BENCH_TIMER_HZ 100000000u

typedef struct {
    int32_t raw[4][SAT1_BENCH_FRAME_SAMPLES];
    int32_t reference[2][SAT1_BENCH_FRAME_SAMPLES];
    uint32_t sequence;
    uint32_t capture_tick;
    uint32_t flags;
} sat1_bench_frame_t;

/* E, W, N, S, reference L/R, processed AGC, pre-AGC NS, sequence16, tick low/high16,
 * flags16. Metadata occupies the high 16 bits, surviving PCM24/float32
 * host audio paths without losing marker bits. No channel gain, clipping or
 * quantization is applied here.
 * Output samples retain the processing stages' own algorithmic delays.
 */
static inline int sat1_bench_interleave(
        int32_t *output, size_t count, const sat1_bench_frame_t *frame,
        const int32_t *processed, const int32_t *ns)
{
    if (output == NULL || frame == NULL || processed == NULL || ns == NULL ||
        count != SAT1_BENCH_FRAME_SAMPLES) return 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t *row = output + i * SAT1_BENCH_CHANNELS;
        for (size_t ch = 0; ch < 4; ++ch) row[ch] = frame->raw[ch][i];
        row[4] = frame->reference[0][i];
        row[5] = frame->reference[1][i];
        row[6] = processed[i];
        row[7] = ns[i];
        uint32_t marker = (frame->sequence & 0xffffu) << 16;
        memcpy(row + 8, &marker, sizeof(marker));
        marker = (frame->capture_tick & 0xffffu) << 16;
        memcpy(row + 9, &marker, sizeof(marker));
        marker = frame->capture_tick & 0xffff0000u;
        memcpy(row + 10, &marker, sizeof(marker));
        marker = (frame->flags & 0xffffu) << 16;
        memcpy(row + 11, &marker, sizeof(marker));
    }
    return 1;
}
