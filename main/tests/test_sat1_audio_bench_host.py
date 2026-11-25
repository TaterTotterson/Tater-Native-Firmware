"""Exercise the actual C capture/packing code and reject damaged lab recordings."""
import importlib.util
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
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
            "app_conf.h": "#pragma once\n#define appconfSAT1_AUDIO_BENCH 1\n#define appconfSAT1_BENCH_PROFILE 1\n#define APP_VERSION_COUNTER 9\n#define THIS_XCORE_TILE 1\n",
            "FreeRTOS.h": "#pragma once\n#include <assert.h>\ntypedef int BaseType_t;\n#define taskENTER_CRITICAL() ((void)0)\n#define taskEXIT_CRITICAL() ((void)0)\n#define configASSERT assert\n#define pdPASS 1\n#define configMINIMAL_STACK_SIZE 32\n#define tskIDLE_PRIORITY 0\n#define pdMS_TO_TICKS(x) (x)\nstatic inline unsigned xPortGetFreeHeapSize(void) { return 1000; }\nstatic inline unsigned xPortGetMinimumEverFreeHeapSize(void) { return 900; }\n",
            "task.h": "#pragma once\nstatic inline void vTaskDelay(unsigned t) {(void)t;}\nstatic inline int xTaskCreate(void (*f)(void *), const char *n, unsigned s, void *a, unsigned p, void *h) { (void)f;(void)n;(void)s;(void)a;(void)p;(void)h;return 1;}\n",
            "rtos_osal.h": "#define RTOS_THREAD_STACK_SIZE(f) 0\n",
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
static unsigned char input[240 * 12 * 4 * 2], written[16 * 12 * 4];
static size_t cursor, calls, drops;
static bool stop_reading;
static void sat1_bench_usb_drop(void) {drops++;}
static int xStreamBufferIsFull(void *s) {(void)s;return 0;}
static void xStreamBufferReset(void *s) {(void)s;}
static size_t xStreamBufferBytesAvailable(void *s) {(void)s;return sizeof(input);}
static size_t xStreamBufferReceive(void *s, void *dest, size_t count, unsigned wait) {
    (void)s;(void)wait;
    assert(++calls < 100);
    if (stop_reading && calls == 2) return 0;
    size_t n = count > 17 ? 17 : count;
    memcpy(dest, input + cursor, n); cursor += n; return n;
}
static void tud_audio_write(const void *data, size_t size) {
    assert(size == sizeof(written)); memcpy(written, data, size);
}
''' + callback + r'''
int main(void) {
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (unsigned char)(i * 37);
    assert(tud_audio_tx_done_pre_load_cb(0, 0, 0, 0));
    assert(memcmp(input, written, sizeof(written)) == 0 && drops == 0);
    stop_reading = true; calls = 0; cursor = 0;
    assert(tud_audio_tx_done_pre_load_cb(0, 0, 0, 0));
    assert(drops == 1 && memcmp(input, written, 17) == 0);
    for (size_t i = 17; i < sizeof(written); ++i) assert(written[i] == 0);
    return 0;
}
''')
        binary = self.directory / "usb_read"
        subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
