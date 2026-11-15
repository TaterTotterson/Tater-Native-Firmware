#!/usr/bin/env python3
"""Compile the production verifier state machine with a clock and transport fake.

FreeRTOS task creation is deliberately unavailable in this harness: scheduling
the verification timeout must not require allocating a worker task or stack.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile


MAIN_DIR = Path(__file__).resolve().parents[1]
source = (MAIN_DIR / "wake_engine.cc").read_text()


def function(signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


production = "\n\n".join(
    function(signature)
    for signature in (
        "void complete_wake_verification(",
        "void poll_wake_verification_timeout()",
        "bool queue_wake_verification(",
        'extern "C" void tater_wake_engine_poll(void)',
    )
)

PRELUDE = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr int portMAX_DELAY = 0;
constexpr int kAudioSampleRate = 16000;
using SemaphoreHandle_t = void*;
SemaphoreHandle_t s_wake_verifier_lock = reinterpret_cast<void*>(1);
uint32_t s_wake_verifier_generation = 0;
uint32_t s_wake_verifier_pending_id = 0;
uint32_t s_wake_verifier_last_id = 0;
uint32_t s_wake_verifier_completed_id = 0;
uint32_t s_wake_verifier_count = 0;
uint32_t s_wake_verifier_rejections = 0;
uint32_t s_wake_verifier_fail_open = 0;
bool s_wake_verifier_pending = false;
int64_t s_wake_verifier_deadline_us = 0;
char s_wake_verifier_pending_word[32] = "";
char s_wake_verifier_last_reason[64] = "";
struct tater_live_settings_t {
    char wake_verifier_mode[16];
    uint16_t wake_verifier_window_ms;
    uint16_t wake_verifier_timeout_ms;
};
int starts = 0;
int resets = 0;
int uploads = 0;
int mutex_depth = 0;
int immediate_reply = 0;
bool upload_ok = true;
int64_t now_us = 1000000;
void complete_wake_verification(uint32_t, bool, bool, const char*);
void xSemaphoreTake(void*, int) { assert(mutex_depth++ == 0); }
void xSemaphoreGive(void*) { assert(--mutex_depth == 0); }
int64_t esp_timer_get_time() { return now_us; }
void tater_protocol_send_log(const char*, const char*) { assert(mutex_depth == 0); }
void start_wake_session(const char*) { assert(mutex_depth == 0); ++starts; }
void tater_wake_engine_reset() { assert(mutex_depth == 0); ++resets; }
void heap_caps_free(void* ptr) { std::free(ptr); }
int16_t* snapshot_capture_audio_tail(size_t, size_t* count) {
    *count = 16;
    return static_cast<int16_t*>(std::calloc(*count, sizeof(int16_t)));
}
uint32_t tater_protocol_send_wake_verification(uint32_t id, const int16_t*, size_t, bool) {
    ++uploads;
    if (immediate_reply != 0) {
        complete_wake_verification(id, immediate_reply > 0, false, "immediate_reply");
    }
    return upload_ok ? id : 0;
}
void clear() {
    s_wake_verifier_generation = s_wake_verifier_pending_id = s_wake_verifier_last_id = 0;
    s_wake_verifier_completed_id = s_wake_verifier_count = s_wake_verifier_rejections = s_wake_verifier_fail_open = 0;
    s_wake_verifier_pending = false;
    s_wake_verifier_deadline_us = 0;
    s_wake_verifier_last_reason[0] = '\0';
    starts = resets = uploads = mutex_depth = immediate_reply = 0;
    upload_ok = true;
    now_us = 1000000;
}
'''

TESTS = r'''
int main() {
    tater_live_settings_t settings = {"enforce", 1000, 500};

    clear();
    assert(queue_wake_verification(&settings, "Hey Juniper", false));
    assert(starts == 0 && s_wake_verifier_pending);
    now_us += 162000;
    tater_wake_engine_poll();
    assert(starts == 0 && s_wake_verifier_count == 0);
    complete_wake_verification(s_wake_verifier_last_id, false, false, "transcript_mismatch");
    now_us += 500000;
    tater_wake_engine_poll();
    assert(starts == 0 && resets == 1);
    assert(s_wake_verifier_count == 1 && s_wake_verifier_rejections == 1 && s_wake_verifier_fail_open == 0);
    assert(s_wake_verifier_deadline_us == 0);
    std::puts("PASS: prompt HA rejection blocks wake without allocating a timeout task");

    clear();
    assert(queue_wake_verification(&settings, "Hey Tater", false));
    complete_wake_verification(s_wake_verifier_last_id, true, false, "matched");
    complete_wake_verification(s_wake_verifier_last_id, true, false, "matched");
    now_us += 1000000;
    tater_wake_engine_poll();
    assert(starts == 1 && s_wake_verifier_count == 1 && s_wake_verifier_fail_open == 0);
    std::puts("PASS: accepted and duplicate replies start exactly one session");

    clear();
    assert(queue_wake_verification(&settings, "Hey Tater", false));
    now_us = s_wake_verifier_deadline_us - 1;
    tater_wake_engine_poll();
    assert(starts == 0);
    ++now_us;
    tater_wake_engine_poll();
    tater_wake_engine_poll();
    assert(starts == 1 && s_wake_verifier_count == 1 && s_wake_verifier_fail_open == 1);
    assert(std::strcmp(s_wake_verifier_last_reason, "satellite_timeout_fail_open") == 0);
    complete_wake_verification(s_wake_verifier_last_id, false, false, "late_rejection");
    assert(starts == 1 && s_wake_verifier_rejections == 0);
    std::puts("PASS: actual timeout keeps fail-open behavior; late replies cannot alter it");

    clear();
    settings.wake_verifier_timeout_ms = 2000;
    assert(queue_wake_verification(&settings, "Hey Tater", false));
    now_us += 1999999;
    tater_wake_engine_poll();
    assert(starts == 0);
    ++now_us;
    tater_wake_engine_poll();
    assert(starts == 1);
    settings.wake_verifier_timeout_ms = 500;
    std::puts("PASS: configured timeout is honored");

    clear();
    assert(queue_wake_verification(&settings, "Hey Tater", false));
    uint32_t previous_id = s_wake_verifier_last_id;
    complete_wake_verification(previous_id, false, false, "transcript_mismatch");
    now_us += 200000;
    assert(queue_wake_verification(&settings, "Hey Tater", false));
    uint32_t current_id = s_wake_verifier_last_id;
    complete_wake_verification(previous_id, true, false, "stale_acceptance");
    assert(starts == 0 && s_wake_verifier_pending_id == current_id);
    now_us += 300000;
    tater_wake_engine_poll();
    assert(starts == 0);
    complete_wake_verification(current_id, false, false, "transcript_mismatch");
    assert(s_wake_verifier_count == 2 && s_wake_verifier_rejections == 2 && s_wake_verifier_fail_open == 0);
    std::puts("PASS: reusable deadline and stale replies cannot finish a newer request");

    for (int reply : {-1, 1}) {
        clear();
        immediate_reply = reply;
        assert(queue_wake_verification(&settings, "Hey Tater", false));
        assert(!s_wake_verifier_pending && s_wake_verifier_deadline_us == 0);
        now_us += 1000000;
        tater_wake_engine_poll();
        assert(s_wake_verifier_count == 1 && s_wake_verifier_fail_open == 0);
        assert(starts == (reply > 0 ? 1 : 0));
    }
    std::puts("PASS: responses during upload setup do not rearm a completed timeout");

    clear();
    upload_ok = false;
    assert(queue_wake_verification(&settings, "Hey Tater", false));
    assert(starts == 1 && s_wake_verifier_deadline_us == 0 && s_wake_verifier_fail_open == 1);
    assert(std::strcmp(s_wake_verifier_last_reason, "queue_fail_open") == 0);
    std::puts("PASS: existing upload-unavailable fail-open behavior is preserved");

    clear();
    std::strcpy(settings.wake_verifier_mode, "observe");
    assert(queue_wake_verification(&settings, "Hey Tater", false));
    assert(!s_wake_verifier_pending && s_wake_verifier_deadline_us == 0);
    now_us += 1000000;
    tater_wake_engine_poll();
    assert(starts == 0 && s_wake_verifier_count == 0);
    complete_wake_verification(s_wake_verifier_last_id, false, false, "observed_mismatch");
    assert(starts == 0 && resets == 0 && s_wake_verifier_rejections == 1);
    std::puts("PASS: Observe mode records results without enforcing or starting a session");

    clear();
    std::strcpy(settings.wake_verifier_mode, "off");
    assert(!queue_wake_verification(&settings, "Hey Tater", false));
    assert(uploads == 0);
    assert(queue_wake_verification(&settings, "Hey Tater", true));
    assert(s_wake_verifier_pending && s_wake_verifier_deadline_us > now_us);
    complete_wake_verification(s_wake_verifier_last_id, false, false, "tv_rejection");
    assert(starts == 0 && resets == 1);
    std::puts("PASS: Off mode and forced TV-profile enforcement remain distinct");
}
'''


with tempfile.TemporaryDirectory(prefix="tater-wake-verifier-") as directory:
    build = Path(directory)
    cpp = build / "test_wake_verifier.cpp"
    binary = build / "test_wake_verifier"
    cpp.write_text(PRELUDE + production + TESTS)
    compiler = shutil.which("c++") or shutil.which("g++")
    if not compiler:
        raise SystemExit("A C++ compiler is required")
    subprocess.run(
        [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-include", "initializer_list", str(cpp), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

# The deadline service must keep running even when no microphone frame arrives.
app = (MAIN_DIR / "app_main.c").read_text()
assert "vTaskDelay(pdMS_TO_TICKS(10));\n        tater_wake_engine_poll();" in app
print("PASS: existing application service loop polls independently of microphone reads")
