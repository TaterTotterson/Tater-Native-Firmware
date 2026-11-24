#!/usr/bin/env python3
"""Check Sat1's production PCM conversion without ESP-IDF or USB hardware."""

import os
from pathlib import Path
import re
import subprocess
import tempfile


MAIN = Path(__file__).resolve().parents[1]
source = (MAIN / "boards/sat1/audio_sat1.c").read_text()
# Compile the actual conversion helpers, without the surrounding I2S drivers.
start = source.index("static int16_t clamp_s16(")
end = source.index("static void mic_level_stats(", start)
defines = "\n".join(
    re.search(rf"^#define {name} .+$", source, re.MULTILINE).group(0)
    for name in (
        "SAT1_WAKE_MIC_GAIN_Q8",
        "SAT1_STREAM_MIC_GAIN_Q8",
        "SAT1_XMOS_WAKE_CHANNEL",
        "SAT1_XMOS_STREAM_CHANNEL",
    )
)
tests = r'''
#include <assert.h>
#include <stdio.h>

int main(void)
{
    int32_t stereo[6];
    for (int32_t value = INT16_MIN; value <= INT16_MAX; ++value) {
        for (size_t frame = 0; frame < 3; ++frame) {
            stereo[frame * 2] = value * 65536;
            stereo[frame * 2 + 1] = value * 65536;
        }
        /* Wake audio keeps its full dynamic range, including loud speech
         * that the former 8x gain would have saturated above +/-4096. */
        assert(sat1_xmos_channel_48k_to_16k(stereo, 0, 0) == value);
        /* Recognition must retain the same full dynamic range as wake audio,
         * without amplifying an already gain-controlled XMOS signal. */
        assert(sat1_xmos_channel_48k_to_16k(stereo, 0, SAT1_XMOS_STREAM_CHANNEL) == value);
    }

    /* Distinct, changing stereo channels check routing and downsampling. */
    int32_t mixed[] = {
        10000 * 65536, 100 * 65536,
        -5000 * 65536, -200 * 65536,
        4000 * 65536, 400 * 65536,
        2000 * 65536, -300 * 65536,
    };
    assert(sat1_xmos_channel_48k_to_16k(mixed, 0, 0) == 3000);
    assert(sat1_xmos_channel_48k_to_16k(mixed, 0, 1) == 100);
    assert(sat1_xmos_channel_48k_to_16k(mixed, 0, SAT1_XMOS_STREAM_CHANNEL) == 3000);
    assert(sat1_xmos_channel_48k_to_16k(mixed, 1, 0) == 333);
    assert(sat1_xmos_channel_48k_to_16k(mixed, 1, 1) == -33);
    /* A quiet pre-AGC channel must not replace audible processed speech. */
    int32_t quiet_ns[] = {
        12000 * 65536, 0,
        12000 * 65536, 0,
        12000 * 65536, 0,
    };
    assert(sat1_xmos_channel_48k_to_16k(quiet_ns, 0, SAT1_XMOS_STREAM_CHANNEL) == 12000);
    puts("Sat1 processed-channel routing and all 65536 unity-gain samples passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="tater-sat1-mic-gain-") as directory:
    root = Path(directory)
    test_source = root / "mic_gain.c"
    test_source.write_text(
        "#include <stdint.h>\n#include <stddef.h>\n"
        "#define TATER_MIC_SOURCE_CHANNELS 2\n"
        + defines + "\n" + source[start:end] + tests
    )
    executable = root / "test-mic-gain"
    subprocess.run(
        [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=undefined", str(test_source), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
