#!/usr/bin/env python3
"""Run the production capture scheduler/uploader against memory and HTTP fakes."""

from pathlib import Path
import shutil
import subprocess
import tempfile

MAIN = Path(__file__).resolve().parents[1]
wake = (MAIN / "wake_engine.cc").read_text()
protocol = (MAIN / "tater_protocol.c").read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


constants = wake[wake.index("constexpr size_t kCaptureBufferSamples"):wake.index("constexpr char kCaptureUploadPath")]
request = wake[wake.index("struct CaptureUploadRequest {"):wake.index("struct PendingCloseMiss {")]
globals_ = wake[wake.index("std::atomic<bool> s_capture_upload_running"):wake.index("PendingCloseMiss s_pending_close_miss")]
production = "\n\n".join(
    [function(protocol, signature) for signature in (
        "bool tater_protocol_audio_busy(void)", "bool tater_protocol_background_work_allowed(void)")]
    + [function(wake, signature) for signature in (
        "uint8_t *snapshot_capture_audio(", "bool capture_upload_may_run()",
        "void free_capture_upload(", "bool requeue_capture_upload(", "void capture_upload_task(",
        "void poll_capture_upload()", "void queue_capture_upload(")]
)

PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 4096
#define configSUPPORT_STATIC_ALLOCATION 1
constexpr int kAudioSampleRate = 16000;
constexpr int portMAX_DELAY = 999;
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4;
constexpr int pdFAIL = 0, pdPASS = 1, ESP_OK = 0, HTTP_METHOD_POST = 1;
using BaseType_t = int;
using esp_err_t = int;
using SemaphoreHandle_t = void*;
enum { TATER_STATE_IDLE, TATER_STATE_LISTENING, TATER_STATE_THINKING,
       TATER_STATE_SPEAKING, TATER_STATE_TOOL_CALL, TATER_STATE_OTA };
bool s_connected = true, s_voice_active = false, s_pending_reopen = false;
int s_current_state = TATER_STATE_IDLE;
bool playing = false, music = false, ringing = false, ota = false, downloading = false;
bool tater_playback_is_playing() { return playing; }
bool tater_sendspin_is_playing() { return music; }
bool timer_any_ringing() { return ringing; }
bool tater_ota_is_running() { return ota; }
bool tater_wake_engine_background_download_active() { return downloading; }
std::atomic<bool> s_boundary_session_running{false};
bool s_wake_verifier_pending = false;
SemaphoreHandle_t s_capture_ring_lock = reinterpret_cast<void*>(1);
SemaphoreHandle_t s_wake_verifier_lock = reinterpret_cast<void*>(2);
int locks = 0;
void xSemaphoreTake(void*, int) { assert(locks++ == 0); }
void xSemaphoreGive(void*) { assert(--locks == 0); }
int64_t now_us = 1000000;
int64_t esp_timer_get_time() { return now_us; }
int16_t ring[48000];
int16_t *s_capture_ring = ring;
size_t s_capture_ring_start = 47000, s_capture_ring_count = 10000;
char s_active_wake_word[32] = "hey_tater", s_last_detection_profile[32] = "balanced";
struct tater_live_settings_t {
    bool capture_wake_audio = true, capture_close_misses = true;
    char trainer_app_url[128] = "http://trainer";
} settings;
const tater_live_settings_t *tater_live_settings_get() { return &settings; }
const char *tater_protocol_device_name() { return "sat1"; }
void build_capture_upload_url(const char *base, char *out, size_t size) {
    std::snprintf(out, size, "%s/api/upload_captured_audio_raw", base);
}
int score_to_raw(float score) { return static_cast<int>(score * 255); }
void tater_protocol_send_log(const char*, const char*) { assert(!locks); }
const char *esp_err_to_name(int) { return "injected failure"; }
unsigned alloc_attempts = 0;
int fail_alloc_after = -1;
std::set<void*> allocations;
void *heap_caps_malloc(size_t size, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ++alloc_attempts;
    if (fail_alloc_after == 0) return nullptr;
    if (fail_alloc_after > 0) --fail_alloc_after;
    void *p = std::malloc(size);
    assert(p && allocations.insert(p).second);
    return p;
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    void *p = heap_caps_malloc(count * size, caps);
    if (p) std::memset(p, 0, count * size);
    return p;
}
void heap_caps_free(void *p) {
    if (p) { assert(allocations.erase(p) == 1); std::free(p); }
}
size_t internal_free = 8000, internal_largest = 4000;
size_t heap_caps_get_free_size(unsigned caps) { assert(caps == MALLOC_CAP_INTERNAL); return internal_free; }
size_t heap_caps_get_largest_free_block(unsigned caps) { assert(caps == MALLOC_CAP_INTERNAL); return internal_largest; }
void (*task_fn)(void*) = nullptr;
void *task_arg = nullptr;
bool task_fail = false;
unsigned task_attempts = 0, finishes = 0;
int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void*), const char*, int stack, void *arg,
                                 int, void*, int, unsigned caps) {
    assert(!task_fn && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) && stack == 8192);
    ++task_attempts;
    if (task_fail) return pdFAIL;
    task_fn = fn; task_arg = arg;
    return pdPASS;
}
void tater_task_delete_current(bool caps) { assert(caps); ++finishes; }
void vTaskDelete(void*) { assert(false); }
void vTaskDelay(int ticks) { assert(ticks == 1); }
void esp_crt_bundle_attach() {}
struct esp_http_client_config_t {
    const char *url;
    int method, timeout_ms, buffer_size, buffer_size_tx;
    void (*crt_bundle_attach)();
};
using esp_http_client_handle_t = void*;
unsigned http_inits = 0, http_opens = 0, http_closes = 0, http_cleanups = 0;
bool client_live = false, fail_http_init = false, fail_http_open = false, fail_http_write = false;
bool interrupt_on_init = false, interrupt_on_write = false;
int status = 200;
std::map<std::string, std::string> headers;
std::vector<uint8_t> uploaded;
void *esp_http_client_init(const esp_http_client_config_t *cfg) {
    ++http_inits;
    assert(!locks && !client_live && cfg->url && cfg->method == HTTP_METHOD_POST);
    assert(cfg->buffer_size > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    assert(cfg->buffer_size_tx > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    assert(cfg->timeout_ms > 0 && cfg->timeout_ms <= 1500);
    if (fail_http_init) return nullptr;
    client_live = true;
    if (interrupt_on_init) s_current_state = TATER_STATE_LISTENING;
    return reinterpret_cast<void*>(3);
}
void esp_http_client_set_header(void*, const char *key, const char *value) { headers[key] = value; }
int esp_http_client_open(void*, size_t bytes) { assert(bytes == 20000); ++http_opens; return fail_http_open ? -1 : ESP_OK; }
int esp_http_client_write(void*, const char *data, size_t size) {
    assert(client_live && size <= 2048);
    if (fail_http_write) return -1;
    uploaded.insert(uploaded.end(), data, data + size);
    if (interrupt_on_write) s_current_state = TATER_STATE_LISTENING;
    return static_cast<int>(size);
}
int esp_http_client_fetch_headers(void*) { return 0; }
int esp_http_client_get_status_code(void*) { return status; }
void esp_http_client_close(void*) { assert(client_live); ++http_closes; }
void esp_http_client_cleanup(void*) { assert(client_live); client_live = false; ++http_cleanups; }
'''

HELPERS = r'''
void set_capture_last_error(const char *message) {
    std::snprintf(s_capture_last_error, sizeof(s_capture_last_error), "%s", message);
}
'''

TESTS = r'''
void queue(const char *event = "wake_detected") {
    queue_capture_upload(event, "hey_tater", .9f, .8f, .7f, 2, 2, "balanced", .85f, .1f, "0.7,0.9");
}
void run_task() {
    assert(task_fn);
    auto fn = task_fn; auto arg = task_arg;
    task_fn = nullptr; task_arg = nullptr;
    fn(arg);
    assert(!s_capture_upload_running && !client_live && !locks);
}
void start_after_idle() {
    poll_capture_upload();
    now_us += kCaptureUploadIdleUs;
    poll_capture_upload();
    assert(task_fn && s_capture_upload_running && !s_pending_capture_upload);
}
void reset() {
    assert(!task_fn && !client_live);
    free_capture_upload(s_pending_capture_upload.exchange(nullptr));
    assert(allocations.empty());
    s_capture_upload_running = false;
    s_capture_idle_since_us = s_capture_retry_after_us = 0;
    settings = tater_live_settings_t{};
    s_current_state = TATER_STATE_IDLE;
    s_connected = true;
    s_voice_active = s_pending_reopen = playing = music = ringing = ota = downloading = false;
    s_wake_verifier_pending = false;
    s_boundary_session_running = false;
    fail_alloc_after = -1;
    internal_free = 8000; internal_largest = 4000;
    task_fail = fail_http_init = fail_http_open = fail_http_write = false;
    interrupt_on_init = interrupt_on_write = false;
    status = 200;
    headers.clear(); uploaded.clear();
    http_inits = http_opens = http_closes = http_cleanups = task_attempts = 0;
    now_us += 100000000;
    for (size_t i = 0; i < 48000; ++i) ring[i] = static_cast<int16_t>(i);
}
int main() {
    reset();
    s_current_state = TATER_STATE_LISTENING;
    queue();
    assert(allocations.size() == 2 && !http_inits && !task_attempts);
    auto *pending = s_pending_capture_upload.load();
    assert(pending && pending->byte_count == 20000);
    std::vector<uint8_t> original(pending->pcm_data, pending->pcm_data + pending->byte_count);
    assert(reinterpret_cast<int16_t*>(pending->pcm_data)[0] == ring[47000]);
    assert(reinterpret_cast<int16_t*>(pending->pcm_data)[1000] == ring[0]);
    std::memset(ring, 0, sizeof(ring)); // Rolling audio cannot change the queued clip.
    auto attempts = alloc_attempts;
    queue();
    assert(alloc_attempts == attempts && s_pending_capture_upload.load() == pending);
    for (int state : {TATER_STATE_LISTENING, TATER_STATE_THINKING, TATER_STATE_SPEAKING,
                      TATER_STATE_TOOL_CALL, TATER_STATE_OTA}) {
        s_current_state = state; now_us += 3000000; poll_capture_upload();
        assert(!task_attempts);
    }
    s_current_state = TATER_STATE_IDLE;
    for (bool *busy : {&s_voice_active, &s_pending_reopen, &playing, &music, &ringing,
                       &ota, &downloading, &s_wake_verifier_pending}) {
        *busy = true; poll_capture_upload(); assert(!task_attempts); *busy = false;
    }
    s_boundary_session_running = true; poll_capture_upload(); assert(!task_attempts);
    s_boundary_session_running = false;
    s_connected = false; poll_capture_upload(); assert(!task_attempts); s_connected = true;
    poll_capture_upload(); now_us += kCaptureUploadIdleUs - 1; poll_capture_upload();
    assert(!task_attempts); ++now_us; poll_capture_upload();
    run_task();
    assert(uploaded == original && allocations.empty());
    assert(http_inits == 1 && http_opens == 1 && http_closes == 1 && http_cleanups == 1);
    assert(headers["X-Event-Type"] == "wake_detected" && headers["X-Source-Device"] == "sat1");
    assert(headers["X-Wake-Word"] == "hey_tater" && headers["X-Probability-History"] == "0.7,0.9");
    puts("PASS: immediate immutable capture, single pending clip, and full voice/audio/OTA idle gating");

    // Foreground work can start before init, while setting headers, or mid-body.
    for (int phase = 0; phase < 3; ++phase) {
        reset(); queue(); start_after_idle();
        if (phase == 0) s_current_state = TATER_STATE_THINKING;
        if (phase == 1) interrupt_on_init = true;
        if (phase == 2) interrupt_on_write = true;
        run_task();
        assert(s_pending_capture_upload && allocations.size() == 2);
        assert(http_closes == http_cleanups && http_cleanups == (phase > 0 ? 1 : 0));
        poll_capture_upload(); // Observe the busy state and reset the idle timer.
        s_current_state = TATER_STATE_IDLE;
        interrupt_on_init = interrupt_on_write = false;
        uploaded.clear(); start_after_idle(); run_task();
        assert(allocations.empty() && uploaded.size() == 20000);
    }
    puts("PASS: voice activity preempts upload safely and preserves one retryable PSRAM clip");

    reset(); queue();
    internal_free = 4095; poll_capture_upload(); now_us += 2000000; poll_capture_upload();
    assert(!task_attempts);
    internal_free = 8000; internal_largest = 2047; poll_capture_upload(); assert(!task_attempts);
    internal_largest = 4000; task_fail = true; poll_capture_upload();
    assert(task_attempts == 1 && s_pending_capture_upload && !s_capture_upload_running);
    now_us += kCaptureUploadRetryUs - 1; poll_capture_upload(); assert(task_attempts == 1);
    task_fail = false; ++now_us; poll_capture_upload(); run_task(); assert(allocations.empty());
    puts("PASS: low-memory/task-creation failure defers without internal-stack fallback; retry is bounded");

    for (int failure = 0; failure < 2; ++failure) {
        reset(); fail_alloc_after = failure; queue();
        assert(!s_pending_capture_upload && allocations.empty() && !task_attempts);
    }
    for (int failure = 0; failure < 4; ++failure) {
        reset(); queue(); start_after_idle();
        if (failure == 0) fail_http_init = true;
        if (failure == 1) fail_http_open = true;
        if (failure == 2) fail_http_write = true;
        if (failure == 3) status = 500;
        run_task();
        assert(allocations.empty() && !s_pending_capture_upload);
        assert(http_cleanups == (failure ? 1 : 0) && http_closes == http_cleanups);
    }
    puts("PASS: PSRAM and HTTP failure paths release ownership exactly once");

    reset(); queue(); settings.capture_wake_audio = false; poll_capture_upload();
    assert(allocations.empty() && !task_attempts);
    reset(); queue(); s_current_state = TATER_STATE_THINKING;
    now_us += kCaptureUploadMaxAgeUs; poll_capture_upload();
    assert(allocations.empty() && !task_attempts);
    reset(); settings.capture_wake_audio = false; queue(); assert(allocations.empty());
    queue("close_miss"); start_after_idle(); run_task();
    assert(headers["X-Event-Type"] == "close_miss" && allocations.empty());
    reset(); queue("close_miss"); settings.capture_close_misses = false; poll_capture_upload();
    assert(allocations.empty() && !task_attempts);
    puts("PASS: disabled/stale captures are freed; close-miss settings and metadata remain independent");
}
'''

with tempfile.TemporaryDirectory(prefix="tater-capture-upload-") as directory:
    build = Path(directory)
    source = build / "capture_test.cpp"
    source.write_text(PRELUDE + constants + request + globals_ + HELPERS + production + TESTS)
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A C++ compiler is required")
    binary = build / "capture_test"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
