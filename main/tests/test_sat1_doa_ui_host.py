"""Run actual native direction rendering/snapshot functions at failure boundaries."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = path.read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class DirectionUiTest(unittest.TestCase):
    def run_c(self, source):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.c"
            path.write_text(source)
            binary = path.with_suffix("")
            subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_legacy_and_tracking_publish_their_own_direction(self):
        src = ROOT / "main/boards/sat1/xmos/source/satellite-xmos-firmware/src"
        path = src / "doa/doa_servicer.c"
        source = path.read_text()
        start = source.index("    case DOA_SERVICER_CMD_READ_STATE:")
        end = source.index("    case DOA_SERVICER_CMD_READ_DIAGNOSTICS:", start)
        body = source[start:end].split(":", 1)[1]
        helpers = (function(path, "static void store_u16_le(") + "\n" +
                   function(path, "static void store_u32_le("))
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include "DOA_HEADER"
#define CONTROL_SUCCESS 0
static unsigned state_reads, observation_reads;
void doa_estimator_get_state(doa_estimator_state_t *out) {
    *out = (doa_estimator_state_t){.confidence=11, .angle_index=7};
    ++state_reads;
}
void doa_estimator_get_observation(doa_estimator_state_t *out) {
    *out = (doa_estimator_state_t){.confidence=33, .angle_index=19};
    ++observation_reads;
}
'''.replace("DOA_HEADER", str(src / "doa/doa_estimator.h"))
        fixture += helpers + "\nstatic int read_state(uint8_t payload[33]) {\n"
        fixture += "uint8_t *payload_ptr = payload + 1;\n" + body + "\n}\n"
        fixture += r'''
int main(void) {
    uint8_t packet[33] = {0};
    assert(read_state(packet) == CONTROL_SUCCESS);
#if appconfSAT1_SPEECH_FRONTEND
    assert(observation_reads == 1 && state_reads == 0);
    assert(packet[3] == 33 && packet[15] == 19);
#else
    assert(state_reads == 1 && observation_reads == 0);
    assert(packet[3] == 11 && packet[15] == 7);
#endif
    return 0;
}
'''
        for tracking in (0, 1):
            with self.subTest(tracking=tracking):
                self.run_c(f"#define appconfSAT1_SPEECH_FRONTEND {tracking}\n" + fixture)

    def test_fractional_south_is_a_real_direction(self):
        actual = function(ROOT / "main/boards/voice_pe/led_ring_voice_pe.c",
                          "static bool current_doa_position(")
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define TATER_BOARD_RESPEAKER_XVF3800 0
#define TATER_BOARD_SAT1 1
#define TATER_LED_COUNT 24
#define SAT1_DOA_MIN_CONFIDENCE 12
typedef struct {
    bool valid, four_mic;
    uint32_t age_ms;
    int16_t sample_delay, vertical_delay, sample_delay_q8, vertical_delay_q8;
    uint8_t angle_index, confidence;
} tater_audio_doa_t;
static tater_audio_doa_t latest;
static bool available = true;
static bool tater_audio_doa_snapshot(tater_audio_doa_t *out) {
    *out = latest;
    return available;
}
static float wrap_position(float value) { return value; }
static float delay_to_position(float value) { return value; }
''' + actual + r'''
int main(void) {
    float position = -1;
    uint8_t confidence = 0;
    latest.valid = latest.four_mic = true;
    latest.confidence = 24;
    latest.vertical_delay_q8 = -96;
    latest.angle_index = 12; /* South; both integer delays rounded to zero. */
    assert(current_doa_position(&position, &confidence));
    assert(position == 12 && confidence == 24);
    latest.age_ms = 301;
    assert(!current_doa_position(&position, &confidence));
    latest.age_ms = 0;
    latest.valid = false;
    assert(!current_doa_position(&position, &confidence));
    latest.valid = true;
    latest.confidence = 11;
    assert(!current_doa_position(&position, &confidence));
    latest.confidence = 24;
    available = false;
    assert(!current_doa_position(&position, &confidence));
    return 0;
}
''')

    def test_update_while_taking_snapshot_is_not_from_future(self):
        actual = function(ROOT / "main/boards/sat1/audio_sat1.c",
                          "bool tater_audio_doa_snapshot(")
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t age_ms; unsigned frame_counter; } tater_audio_doa_t;
static tater_audio_doa_t s_doa;
static int64_t s_doa_update_us, now;
static int s_doa_lock;
static bool update_on_lock;
static void enter_lock(int *lock) {
    (void)lock;
    if (update_on_lock) {
        now += 1000;
        s_doa_update_us = now;
        ++s_doa.frame_counter;
        update_on_lock = false;
    }
}
#define portENTER_CRITICAL(lock) enter_lock(lock)
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int64_t esp_timer_get_time(void) { return now; }
''' + actual + r'''
int main(void) {
    tater_audio_doa_t snapshot;
    now = 1000000;
    s_doa_update_us = 999000;
    update_on_lock = true;
    assert(tater_audio_doa_snapshot(&snapshot));
    assert(snapshot.frame_counter == 1 && snapshot.age_ms == 0);
    now += 451000;
    assert(tater_audio_doa_snapshot(&snapshot) && snapshot.age_ms == 451);
    s_doa_update_us = 0;
    assert(!tater_audio_doa_snapshot(&snapshot));
    assert(!tater_audio_doa_snapshot(NULL));
    return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
