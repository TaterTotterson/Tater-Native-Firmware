"""Exercise the actual C capture/packing code and reject damaged lab recordings."""
import importlib.util
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import wave

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "main/boards/sat1/xmos/source/satellite-xmos-firmware/src"
spec = importlib.util.spec_from_file_location("sat1_audio_bench", ROOT / "scripts/sat1_audio_bench.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class AudioBenchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.tmp.name)
        compiler = shutil.which("cc")
        if not compiler:
            raise RuntimeError("A host C compiler is required")
        # Boundary stubs let the production snapshot implementation run on host.
        stubs = {
            "app_conf.h": "#pragma once\n#define appconfSAT1_AUDIO_BENCH 1\n#define appconfSAT1_BENCH_PROFILE 1\n#define APP_VERSION_COUNTER 9\n#define THIS_XCORE_TILE 1\n#define ON_TILE(n) ((n) == THIS_XCORE_TILE)\n",
            "FreeRTOS.h": "#pragma once\n#include <assert.h>\ntypedef int BaseType_t;\n#define taskENTER_CRITICAL() ((void)0)\n#define taskEXIT_CRITICAL() ((void)0)\n#define configASSERT assert\n#define pdPASS 1\n#define portMAX_DELAY 0xffffffffu\n#define configMINIMAL_STACK_SIZE 32\n#define tskIDLE_PRIORITY 0\n#define pdMS_TO_TICKS(x) (x)\nstatic inline unsigned xPortGetFreeHeapSize(void) { return 1000; }\nstatic inline unsigned xPortGetMinimumEverFreeHeapSize(void) { return 900; }\n",
            "task.h": "#pragma once\nstatic inline void vTaskDelay(unsigned t) {(void)t;}\nstatic inline int xTaskCreate(void (*f)(void *), const char *n, unsigned s, void *a, unsigned p, void *h) { (void)f;(void)n;(void)s;(void)a;(void)p;(void)h;return 1;}\n",
            "rtos_osal.h": "#define RTOS_THREAD_STACK_SIZE(f) 0\n",
            "queue.h": "#pragma once\n#include <string.h>\ntypedef struct {void *items[4];unsigned count,capacity;} fake_queue_t;\ntypedef fake_queue_t *QueueHandle_t;\nstatic inline QueueHandle_t xQueueCreate(unsigned n, unsigned size) {static fake_queue_t q;(void)size;q.count=0;q.capacity=n;return &q;}\nstatic inline int xQueueSend(QueueHandle_t q,const void *item,unsigned timeout) {(void)timeout;if(q->count==q->capacity)return 0;memcpy(&q->items[q->count++],item,sizeof(void *));return 1;}\nstatic inline int xQueueReceive(QueueHandle_t q,void *item,unsigned timeout) {(void)timeout;if(!q->count)return 0;memcpy(item,&q->items[0],sizeof(void *));memmove(q->items,q->items+1,--q->count*sizeof(void *));return 1;}\n",
            "usb/usb_cdc.h": "static inline int cdc_printf(const char *format, ...) {(void)format; return 0;}\n",
            "xcore/hwtimer.h": "#pragma once\n#include <stdint.h>\nstatic inline uint32_t get_reference_time(void) { static uint32_t t = 0xfff00000; t += 1500000; return t; }\n",
        }
        for name, content in stubs.items():
            path = cls.directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        source = cls.directory / "capture.c"
        source.write_text(r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "bench/audio_bench.h"
int main(void) {
    int32_t raw[4][240], left[240], right[240], agc[240], ns[240];
    sat1_bench_frame_t frame;
    int32_t packed[240][12];
    sat1_bench_pool_init();
    void *slot0 = sat1_bench_frame_acquire(SAT1_BENCH_PIPELINE_FRAME_BYTES);
    void *slot1 = sat1_bench_frame_acquire(SAT1_BENCH_PIPELINE_FRAME_BYTES);
    assert(slot0 != slot1 && ((uintptr_t)slot0 & 7u) == 0 && ((uintptr_t)slot1 & 7u) == 0);
    memset(slot0, 0xa5, SAT1_BENCH_PIPELINE_FRAME_BYTES);
    memset(slot1, 0x5a, SAT1_BENCH_PIPELINE_FRAME_BYTES);
    assert(((unsigned char *)slot0)[0] == 0xa5);
    sat1_bench_frame_release(slot0);
    assert(sat1_bench_frame_acquire(SAT1_BENCH_PIPELINE_FRAME_BYTES) == slot0);
    sat1_bench_frame_release(slot0);
    sat1_bench_frame_release(slot1);
    for (int i = 0; i < 240; ++i) {
        for (int ch = 0; ch < 4; ++ch) raw[ch][i] = (ch + 1) * 1000000 + i;
        left[i] = 500000000 + i; right[i] = -300000000 - i;
        agc[i] = i == 0 ? INT32_MIN : i * 100000; ns[i] = INT32_MAX - i;
    }
    sat1_bench_arm_capture();
    sat1_bench_note_raw(&raw[0][0], 240);
    sat1_bench_snapshot(&frame, left, right);
    assert(frame.flags == 0x0917 && frame.sequence == 1);
    assert(frame.raw[3][239] == raw[3][239]);
    assert(frame.reference[0][239] == left[239]);
    assert(frame.reference[1][239] == right[239]);
    raw[0][0] = 123;
    assert(frame.raw[0][0] == 1000000);
    sat1_bench_arm_capture();
    sat1_bench_snapshot(&frame, left, right);
    assert(!(frame.flags & SAT1_BENCH_RAW_VALID) && frame.raw[0][0] == 0);
    sat1_bench_note_raw(&raw[0][0], 239);
    sat1_bench_snapshot(&frame, left, right);
    assert(!(frame.flags & SAT1_BENCH_RAW_VALID));
    sat1_bench_arm_capture();
    sat1_bench_note_raw(&raw[0][0], 240);
    sat1_bench_snapshot(&frame, left, right);
    assert(!sat1_bench_interleave(&packed[0][0], 239, &frame, agc, ns));
    assert(!sat1_bench_interleave(NULL, 240, &frame, agc, ns));
    for (unsigned i = 0; i < 6; ++i) {
        frame.sequence = 65534u + i;
        frame.capture_tick = 0xfff00000u + i * 1500000u;
        assert(sat1_bench_interleave(&packed[0][0], 240, &frame, agc, ns));
        assert(packed[0][6] == INT32_MIN && packed[0][7] == INT32_MAX);
        assert(fwrite(packed, sizeof(packed), 1, stdout) == 1);
    }
    return 0;
}
''')
        binary = cls.directory / "capture"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(cls.directory), "-I", str(SRC), str(source),
                        str(SRC / "bench/audio_bench.c"), "-o", str(binary)], check=True)
        cls.pcm = subprocess.check_output([str(binary)])
        if sys.byteorder != "little":
            raise RuntimeError("Host fixture writes native-endian data; run this fixture on a little-endian host")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def report(self, data=None, **kwargs):
        path = self.directory / "fixture.wav"
        with wave.open(str(path), "wb") as wav:
            wav.setparams((12, 4, 16000, 0, "NONE", "not compressed"))
            wav.writeframes(self.pcm if data is None else data)
        return bench.analyze(path, **kwargs)

    def test_actual_c_capture_and_wraparound(self):
        report = self.report(expected={"bench_profile": 1, "linked_version": [1, 2, 0, 4, 9]})
        self.assertTrue(report["transport_valid"], report)
        self.assertEqual(report["complete_dsp_frames"], 6)
        self.assertEqual(report["capture_intervals_ms"]["mean"], 15)
        self.assertNotEqual(report["audio"]["reference_l"]["mean"], report["audio"]["reference_r"]["mean"])
        self.assertEqual(report["audio"]["processed_agc"]["minimum"], -2147483648)

    def test_trimmed_edges_and_startup_zeros(self):
        report = self.report(bytes(16 * 48) + self.pcm[60 * 48:-60 * 48])
        self.assertTrue(report["transport_valid"], report)
        self.assertEqual(report["startup_samples_ignored"], 16)
        self.assertEqual(report["complete_dsp_frames"], 4)

    def test_missing_frame(self):
        size = 240 * 48
        self.assertFalse(self.report(self.pcm[:size] + self.pcm[2 * size:])["transport_valid"])

    def test_missing_sample(self):
        self.assertFalse(self.report(self.pcm[:300 * 48] + self.pcm[301 * 48:])["transport_valid"])

    def test_repeated_or_frozen_frame(self):
        size = 240 * 48
        self.assertFalse(self.report(self.pcm[:size] + self.pcm)["transport_valid"])

    def test_underflow_silence_inside_capture(self):
        data = self.pcm[:300 * 48] + bytes(16 * 48) + self.pcm[300 * 48:]
        self.assertFalse(self.report(data)["transport_valid"])

    def test_host_changes_marker_bits(self):
        data = bytearray(self.pcm)
        struct.pack_into("<i", data, 8 * 4, 123)
        self.assertFalse(self.report(data)["transport_valid"])

    def test_pcm24_host_preserves_markers(self):
        words = struct.unpack(f"<{len(self.pcm) // 4}i", self.pcm)
        quantized = [value & ~255 for value in words]
        self.assertTrue(self.report(struct.pack(f"<{len(words)}i", *quantized))["transport_valid"])

    def test_wrong_image_identity(self):
        self.assertFalse(self.report(expected={"bench_profile": 3, "linked_version": [1, 2, 0, 4, 10]})["transport_valid"])

    def test_all_silence_is_not_valid_transport(self):
        self.assertFalse(self.report(bytes(len(self.pcm)))["transport_valid"])

    def test_duration_validation(self):
        for value in ("0", "-1", "nan", "inf", "3601"):
            with self.assertRaises(Exception):
                bench.positive(value)

    def test_build_helper_rejects_ambiguous_bench_setting(self):
        result = subprocess.run(["bash", str(SRC.parents[1] / "build_sat1_fixed_delay.sh")],
            env={**os.environ, "SAT1_XMOS_AUDIO_BENCH": "ON"}, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SAT1_XMOS_AUDIO_BENCH must be 0 or 1", result.stderr)

    def test_bench_audio_uses_a_link_independent_of_i2s_rpc(self):
        # Preprocess the actual config/callbacks in both layouts and modes.
        # A shared TX lock allows an audio send waiting on a full frame pool to
        # block the I2S reply needed by output to return a frame to that pool.
        config = re.sub(r'^#include .*$', '', (SRC / "app_conf.h").read_text(), flags=re.M)
        pipelines = SRC.parent / "audio_pipelines/reference"
        for enabled in (0, 1):
            expected = "intertile_usb_audio_ctx" if enabled else "intertile_ctx"
            for layout in ("fixed_delay", "fixed_delay4"):
                source = config + "\n" + "\n".join(
                    re.sub(r'^#include .*$', '', (pipelines / layout / f"audio_pipeline_t{tile}.c").read_text(), flags=re.M)
                    for tile in (0, 1))
                result = subprocess.run([shutil.which("cc"), "-E", "-P", "-x", "c",
                    f"-DappconfSAT1_AUDIO_BENCH={enabled}", "-DON_TILE(n)=1",
                    "-DMIC_ARRAY_CONFIG_MIC_COUNT=4", "-DMIC_ARRAY_CONFIG_SAMPLES_PER_FRAME=240", "-"], input=source,
                    text=True, capture_output=True, check=True)
                calls = re.findall(r'rtos_intertile_(?:tx|rx_len|rx_data)\(\s*(\w+)', result.stdout)
                self.assertEqual(calls, [expected] * 3, (layout, enabled, calls))

    def test_speaker_buffers_remain_stereo_with_four_microphones(self):
        source = (SRC.parent / "audio_pipelines/speaker/passthrough/speaker_pipeline.c").read_text()
        callbacks = source[source.index("#if appconfAUDIO_PIPELINE_FRAME_ADVANCE"):source.index("void empty_stage")]
        harness = self.directory / "speaker_channels.c"
        harness.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define appconfAUDIO_PIPELINE_FRAME_ADVANCE 240
#define appconfAUDIO_SPK_PIPELINE_FRAME_ADVANCE (240 * RATE_FACTOR)
#define appconfAUDIO_SPK_PL_SR_FACTOR RATE_FACTOR
#define configASSERT assert
static void *pvPortMalloc(size_t bytes) {
    assert(bytes == 2 * 240 * RATE_FACTOR * sizeof(int32_t));
    return malloc(bytes);
}
static void speaker_pipeline_input(void *ctx, int32_t **samples, size_t channels, size_t frames) {
    (void)ctx;
    assert(channels == 2 && frames == 240 * RATE_FACTOR);
    int32_t *data = (int32_t *)samples;
    for (size_t i = 0; i < frames; ++i) {data[i] = (int32_t)i + 1; data[frames + i] = -(int32_t)i - 7;}
}
static int speaker_pipeline_output(void *ctx, int32_t **samples, size_t channels, size_t frames) {
    (void)ctx;
    assert(channels == 2 && frames == 240 * RATE_FACTOR);
    int32_t *data = (int32_t *)samples;
    for (size_t i = 0; i < frames; ++i) assert(data[i] == (int32_t)i + 1 && data[frames + i] == -(int32_t)i - 7);
    return 1;
}
''' + callbacks + r'''
int main(void) {
    frame_data_t *frame = audio_pipeline_input_i(NULL);
    assert(audio_pipeline_output_i(frame, NULL) == 1);
    free(frame);
    return 0;
}
''')
        for microphones in (2, 4):
            for rate_factor in (1, 3):
                binary = self.directory / f"speaker_channels_{microphones}_{rate_factor}"
                subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", f"-DappconfAUDIO_PIPELINE_CHANNELS={microphones}",
                    f"-DRATE_FACTOR={rate_factor}", str(harness), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True, timeout=10)

    def test_post_beam_does_not_wait_for_in_progress_direction_analysis(self):
        source = (SRC / "main.c").read_text()

        def function(signature):
            start = source.index(signature)
            opening = source.index("{", start)
            depth, end = 1, opening + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[start:end]

        (self.directory / "sat1_beam_callbacks.inc").write_text(
            function("static size_t receive_mic_array_frame(") + "\n" +
            function("void sat1_post_aec_beam("))
        binary = self.directory / "beam_lock"
        subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-pthread", "-fsanitize=address,undefined", "-I", str(self.directory), "-I", str(SRC),
            str(ROOT / "main/tests/test_sat1_beam_lock_host.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)

    def test_cdc_rpc_prints_are_serialized_through_the_response(self):
        source = (SRC / "usb/usb_cdc.c").read_text()

        def function(signature):
            start = source.index(signature)
            opening = source.index("{", start)
            depth, end = 1, opening + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[start:end]

        (self.directory / "sat1_cdc_callbacks.inc").write_text(
            function("int cdc_printf(") + "\n" +
            function("void rtos_cdc_rpc_client_init("))
        binary = self.directory / "cdc_lock"
        subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-pthread", "-fsanitize=address,undefined", "-I", str(self.directory),
            str(ROOT / "main/tests/test_sat1_cdc_lock_host.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)

    def test_cdc_rpc_link_is_independent_of_audio_control_on_both_tiles(self):
        platform = SRC.parent / "bsp_config/SATELLITE1/platform"

        def function(path, signature):
            source = path.read_text()
            start = source.index(signature)
            opening = source.index("{", start)
            depth, end = 1, opening + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[start:end]

        source = "\n".join([
            function(platform / "platform_init.c", "static void usb_cdc_init("),
            function(platform / "platform_init.c", "void platform_init("),
            function(platform / "platform_start.c", "void platform_start(")])
        for tile in (0, 1):
            for cdc in (0, 1):
                for usb_audio in (0, 1):
                    result = subprocess.run([shutil.which("cc"), "-E", "-P", "-x", "c",
                        "-DUSB_TILE_NO=0", f"-DTILE_UNDER_TEST={tile}",
                        "-DON_TILE(n)=((n)==TILE_UNDER_TEST)",
                        f"-DappconfUSB_CDC_ENABLED={cdc}",
                        f"-DappconfUSB_AUDIO_ENABLED={usb_audio}", "-"],
                        input=source, text=True, capture_output=True, check=True).stdout
                    compact = re.sub(r'\s+', '', result)
                    self.assertEqual(compact.count("rtos_intertile_init(intertile_cdc_ctx,"), cdc)
                    self.assertEqual(compact.count("rtos_intertile_start(intertile_cdc_ctx)"), cdc)
                    self.assertIn("rtos_intertile_init(intertile_ctx,", compact)
                    self.assertIn("rtos_intertile_start(intertile_ctx)", compact)
                    self.assertEqual(compact.count("rtos_intertile_init(intertile_usb_audio_ctx,"), usb_audio)
                    if cdc and tile == 0:
                        self.assertIn("client_intertile_ctx[1]={intertile_cdc_ctx}", compact)
                        self.assertIn("rtos_cdc_rpc_host_init(client_intertile_ctx,1)", compact)
                    elif cdc:
                        self.assertIn("rtos_cdc_rpc_client_init(intertile_cdc_ctx)", compact)
                    else:
                        self.assertNotIn("rtos_cdc_rpc_host_init(", compact)
                        self.assertNotIn("rtos_cdc_rpc_client_init(", compact)

    def capture_fixture(self, name, overflow=False):
        output = self.directory / f"{name}.wav"
        manifest = self.directory / "bench-manifest.json"
        manifest.write_text(json.dumps({"beam_ic": {
            "audio_bench": True, "bench_format": 1, "capture_channels": 12,
            "capture_sample_rate": 16000, "bench_profile": 1,
            "linked_version": [1, 2, 0, 4, 9]}}))
        pcm = self.pcm

        class Stream:
            def __init__(self, **kwargs):
                assert kwargs["dither_off"] is True
                self.offset = 0

            def __enter__(self): return self
            def __exit__(self, *args): pass

            def read(self, count):
                data = pcm[self.offset:self.offset + count * 48]
                self.offset += count * 48
                return data, overflow and self.offset == count * 48

        args = SimpleNamespace(output=output, manifest=manifest, profile="beam_ic",
                               case="quiet", notes="fixture", device="1", seconds=0.09)
        with patch.dict(sys.modules, {"sounddevice": SimpleNamespace(RawInputStream=Stream)}), redirect_stdout(io.StringIO()):
            result = bench.record(args)
            with self.assertRaises(ValueError):
                bench.record(args)
        return result, json.loads(output.with_suffix(".report.json").read_text())

    def test_record_with_native_format_and_exclusive_files(self):
        status, report = self.capture_fixture("record-good")
        self.assertEqual(status, 0)
        self.assertTrue(report["transport_valid"], report)

    def test_record_host_overflow_cannot_pass(self):
        status, report = self.capture_fixture("record-overflow", overflow=True)
        self.assertEqual(status, 2)
        self.assertFalse(report["transport_valid"])

    def test_usb_partial_reads_use_byte_offsets_and_cannot_spin(self):
        source = (SRC / "usb/usb_audio.c").read_text()
        start = source.index("bool tud_audio_tx_done_pre_load_cb(")
        end = source.index("bool tud_audio_tx_done_post_load_cb(", start)
        callback = source[start:end]
        harness = self.directory / "usb_read.c"
        harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX 12
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX 2
#define AUDIO_FRAMES_PER_USB_MICS_FRAME 16
#define RATE_MULTIPLIER 1
#define appconfAUDIO_PIPELINE_FRAME_ADVANCE 240
#define appconfSAT1_AUDIO_BENCH 1
#define SRC_FF3V_FIR_TAPS_PER_PHASE 1
#define xassert assert
#define rtos_printf(...) ((void)0)
typedef int32_t samp_t;
static int32_t src_ff3v_fir_coefs[3][1];
static int32_t src_us3_voice_input_sample(int32_t *s, const int32_t *c, int32_t v) {(void)s;(void)c;return v;}
static int32_t src_us3_voice_get_next_sample(int32_t *s, const int32_t *c) {(void)s;(void)c;return 0;}
static unsigned prev_n_bytes_received;
static bool host_streaming_out, mic_interface_open = true;
static void *samples_to_host_stream_buf;
static unsigned char input[240 * 12 * 4 * 2], written[17 * 12 * 4];
static size_t cursor, calls, drops, resets, written_size;
static size_t available = sizeof(input);
static bool stop_reading, full, drift_run;
static void sat1_bench_usb_drop(void) {drops++;}
#define xStreamBufferIsFull(s) full
#define xStreamBufferReset(s) (resets++)
static size_t xStreamBufferBytesAvailable(void *s) {(void)s;return available;}
static size_t xStreamBufferReceive(void *s, void *dest, size_t count, unsigned wait) {
    (void)s;(void)wait;
    ++calls;
    if (stop_reading && calls == 2) return 0;
    size_t n = count > 17 ? 17 : count;
    assert(n <= available);
    if (drift_run) {
        for (size_t i = 0; i < n; ++i) ((unsigned char *)dest)[i] = (unsigned char)((cursor + i) * 37);
    } else {
        memcpy(dest, input + cursor, n);
    }
    cursor += n; available -= n; return n;
}
static uint16_t tud_audio_write(const void *data, size_t size) {
    assert(size <= sizeof(written)); written_size = size;
    memcpy(written, data, size); return (uint16_t)size;
}
''' + callback + r'''
int main(void) {
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (unsigned char)(i * 37);
    assert(tud_audio_tx_done_pre_load_cb(0, 0, 0, 0));
    assert(written_size == 16 * 12 * 4 && memcmp(input, written, written_size) == 0 && drops == 0);
    stop_reading = true; calls = 0; cursor = 0; available = sizeof(input);
    assert(tud_audio_tx_done_pre_load_cb(0, 0, 0, 0));
    assert(drops == 1 && memcmp(input, written, 17) == 0);
    for (size_t i = 17; i < written_size; ++i) assert(written[i] == 0);
    /* A full capture buffer must drain, preserving its samples. */
    stop_reading = false; full = true; calls = cursor = drops = 0;
    available = 3 * 240 * 12 * 4;
    assert(tud_audio_tx_done_pre_load_cb(0, 0, 0, 0));
    assert(resets == 0 && drops == 0 && written_size == 17 * 12 * 4);
    assert(memcmp(input, written, written_size) == 0);
    /* Starvation is reported even when the callback can pad a packet. */
    full = false; calls = cursor = drops = 0; available = 8 * 12 * 4;
    assert(tud_audio_tx_done_pre_load_cb(0, 0, 0, 0));
    assert(drops == 1 && written_size == 15 * 12 * 4);
    for (size_t i = 8 * 12 * 4; i < written_size; ++i) assert(written[i] == 0);
    /* Run the actual callback against frame-sized arrivals from clocks at
     * +/-2000 ppm for 30 seconds each. Check every byte and bounded fill. */
    drift_run = true;
    for (unsigned rate = 15968; rate <= 16032; rate += 64) {
        unsigned phase = 0;
        size_t emitted = 0;
        calls = cursor = drops = 0; available = 2 * 240 * 12 * 4;
        for (unsigned ms = 0; ms < 30000; ++ms) {
            phase += rate;
            if (phase >= 240000) {
                available += 240 * 12 * 4;
                phase -= 240000;
            }
            assert(available <= 3 * 240 * 12 * 4);
            assert(tud_audio_tx_done_pre_load_cb(0, 0, 0, 0));
            assert(written_size >= 15 * 12 * 4 && written_size <= 17 * 12 * 4);
            for (size_t i = 0; i < written_size; ++i) assert(written[i] == (unsigned char)((emitted + i) * 37));
            emitted += written_size;
            assert(drops == 0 && resets == 0);
        }
    }
    return 0;
}
''')
        binary = self.directory / "usb_read"
        subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
