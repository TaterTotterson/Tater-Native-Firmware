#include "wake_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern "C" {
#include "frontend.h"
#include "frontend_util.h"
#include "native_settings.h"
#include "ota_update.h"
#include "playback.h"
#include "tater_protocol.h"
#include "wake_model_assets.h"
#include "wake_sound_assets.h"
}

namespace {

constexpr char TAG[] = "tater_wake";
constexpr int kAudioSampleRate = 16000;
constexpr int kFeatureDurationMs = 30;
constexpr int kFeatureStepMs = 10;
constexpr int kFeatureSize = 40;
constexpr int kWakeInputFeatureFrames = 2;
constexpr int kWakeInputElements = kWakeInputFeatureFrames * kFeatureSize;
constexpr int kMaxSlidingWindow = 10;
constexpr int64_t kCloseMissConfirmationDelayUs = 900000;
constexpr int64_t kCloseMissUploadCooldownUs = 10000000;
constexpr size_t kWakeArenaSize = 192 * 1024;
constexpr int kWakeResourceVariableCount = 8;
constexpr float kFilterbankLowerBandLimit = 125.0f;
constexpr float kFilterbankUpperBandLimit = 7500.0f;
constexpr int kNoiseReductionSmoothingBits = 10;
constexpr float kNoiseReductionEvenSmoothing = 0.025f;
constexpr float kNoiseReductionOddSmoothing = 0.06f;
constexpr float kNoiseReductionMinSignalRemaining = 0.05f;
constexpr bool kPcanGainControlEnablePcan = true;
constexpr float kPcanGainControlStrength = 0.95f;
constexpr float kPcanGainControlOffset = 80.0f;
constexpr int kPcanGainControlGainBits = 21;
constexpr bool kLogScaleEnableLog = true;
constexpr int kLogScaleScaleShift = 6;
constexpr int32_t kFrontendValueScale = 256;
constexpr int32_t kFrontendValueDiv = 666;
constexpr size_t kWakeManifestMaxBytes = 16 * 1024;
constexpr size_t kWakeDownloadedModelMaxBytes = 512 * 1024;
constexpr size_t kCaptureBufferSamples = kAudioSampleRate * 3;
constexpr size_t kCaptureUploadBufferSize = 2048;
constexpr int kCaptureUploadTimeoutMs = 15000;
constexpr char kCaptureUploadPath[] = "/api/upload_captured_audio_raw";

using WakeOpResolver = tflite::MicroMutableOpResolver<14>;

enum class WakeEnvironment : uint8_t {
    FAR_FIELD = 0,
    BALANCED,
    STRICT,
    TV_NEARBY,
};

struct DownloadedWakeModel {
    uint8_t *data;
    size_t size;
    char id[32];
    char label[48];
    char source_url[256];
    char model_url[256];
};

struct WakeDownloadRequest {
    char url[256];
};

struct CaptureUploadRequest {
    uint8_t *pcm_data;
    size_t byte_count;
    char upload_url[256];
    char source_device[64];
    char wake_word[32];
    char event_type[16];
    char detection_profile[32];
    char probability_history[128];
    float max_probability;
    float average_probability;
    float probability_cutoff;
    float peak_probability_cutoff;
    float rise_score;
    uint8_t active_window_count;
    uint8_t min_active_windows;
};

struct PendingCloseMiss {
    bool active;
    int64_t due_us;
    char wake_word[32];
    char detection_profile[32];
    char probability_history[128];
    float max_probability;
    float average_probability;
    float probability_cutoff;
    float peak_probability_cutoff;
    float rise_score;
    uint8_t active_window_count;
    uint8_t min_active_windows;
};

uint8_t *s_wake_arena = nullptr;
bool s_wake_arena_psram = false;

const tflite::Model *s_wake_model = nullptr;
tflite::MicroInterpreter *s_wake_interpreter = nullptr;
tflite::MicroAllocator *s_wake_allocator = nullptr;
tflite::MicroResourceVariables *s_wake_resources = nullptr;
WakeOpResolver s_wake_resolver;
bool s_wake_resolver_ready = false;
alignas(tflite::MicroInterpreter) uint8_t s_wake_interpreter_storage[sizeof(tflite::MicroInterpreter)];
bool s_wake_interpreter_constructed = false;
FrontendConfig s_frontend_config;
FrontendState s_frontend_state;
bool s_frontend_ready = false;

int8_t s_feature_history[kWakeInputElements];
float s_score_window[kMaxSlidingWindow];
size_t s_feature_history_frames = 0;
size_t s_score_window_count = 0;
size_t s_score_window_index = 0;
int64_t s_cooldown_until_us = 0;
uint32_t s_feature_frames = 0;
uint32_t s_inference_count = 0;
uint32_t s_detection_count = 0;
size_t s_wake_arena_used = 0;
float s_last_score = 0.0f;
float s_last_average = 0.0f;
float s_last_peak = 0.0f;
float s_last_peak_threshold = 0.0f;
float s_last_rise_score = 0.0f;
uint8_t s_last_active_window_count = 0;
uint8_t s_last_min_active_windows = 0;
int32_t s_last_raw_score = 0;
char s_last_detection_profile[32] = "balanced";
char s_last_reject_reason[32] = "";
char s_last_error[96] = "not initialized";
bool s_ready = false;
bool s_runtime_was_enabled = false;
char s_active_wake_word[32] = "hey_tater";
char s_active_wake_label[48] = "Hey Tater";
char s_pending_wake_word[32] = "hey_tater";
char s_active_model_source[16] = "embedded";
char s_active_model_url[256] = "";
uint8_t *s_active_custom_model_data = nullptr;
size_t s_active_custom_model_size = 0;
SemaphoreHandle_t s_custom_model_lock = nullptr;
DownloadedWakeModel s_pending_custom_model = {};
char s_requested_custom_url[256] = "";
char s_downloading_custom_url[256] = "";
bool s_custom_download_running = false;
uint32_t s_custom_download_failures = 0;
uint64_t s_audio_samples_seen = 0;
uint32_t s_audio_chunks_seen = 0;
uint32_t s_audio_nonzero_chunks = 0;
float s_audio_last_rms = 0.0f;
float s_audio_last_abs_mean = 0.0f;
int32_t s_audio_last_peak = 0;
int32_t s_audio_last_min = 0;
int32_t s_audio_last_max = 0;
int64_t s_audio_last_update_us = 0;
int64_t s_last_debug_log_us = 0;
int32_t s_feature_last_min = 0;
int32_t s_feature_last_max = 0;
float s_feature_last_mean = 0.0f;
int16_t *s_capture_ring = nullptr;
size_t s_capture_ring_start = 0;
size_t s_capture_ring_count = 0;
bool s_capture_ring_psram = false;
volatile bool s_capture_upload_running = false;
uint32_t s_capture_upload_count = 0;
uint32_t s_capture_upload_failures = 0;
char s_capture_last_event_type[16] = "";
char s_capture_last_error[96] = "";
PendingCloseMiss s_pending_close_miss = {};
int64_t s_last_close_miss_upload_us = 0;

void set_last_error(const char *message)
{
    std::snprintf(s_last_error, sizeof(s_last_error), "%s", message ? message : "unknown");
}

void set_last_errorf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vsnprintf(s_last_error, sizeof(s_last_error), format, args);
    va_end(args);
}

void log_tensor(const char *label, const TfLiteTensor *tensor);

bool ensure_wake_arena()
{
    if (s_wake_arena) {
        return true;
    }

    s_wake_arena = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, kWakeArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_wake_arena_psram = s_wake_arena != nullptr;
    if (!s_wake_arena) {
        s_wake_arena = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, kWakeArenaSize, MALLOC_CAP_8BIT));
    }
    if (!s_wake_arena) {
        set_last_errorf("wake arena alloc failed size=%u", (unsigned)kWakeArenaSize);
        ESP_LOGE(TAG, "wake arena alloc failed size=%u", (unsigned)kWakeArenaSize);
        return false;
    }

    std::memset(s_wake_arena, 0, kWakeArenaSize);
    return true;
}

bool check_status(TfLiteStatus status, const char *label)
{
    if (status == kTfLiteOk) {
        return true;
    }
    set_last_error(label);
    ESP_LOGE(TAG, "%s failed", label);
    return false;
}

bool register_wake_ops(WakeOpResolver &resolver)
{
    return check_status(resolver.AddCallOnce(), "wake AddCallOnce") &&
        check_status(resolver.AddVarHandle(), "wake AddVarHandle") &&
        check_status(resolver.AddReadVariable(), "wake AddReadVariable") &&
        check_status(resolver.AddAssignVariable(), "wake AddAssignVariable") &&
        check_status(resolver.AddReshape(), "wake AddReshape") &&
        check_status(resolver.AddConcatenation(), "wake AddConcatenation") &&
        check_status(resolver.AddStridedSlice(), "wake AddStridedSlice") &&
        check_status(resolver.AddConv2D(), "wake AddConv2D") &&
        check_status(resolver.AddDepthwiseConv2D(), "wake AddDepthwiseConv2D") &&
        check_status(resolver.AddSplitV(), "wake AddSplitV") &&
        check_status(resolver.AddFullyConnected(), "wake AddFullyConnected") &&
        check_status(resolver.AddLogistic(), "wake AddLogistic") &&
        check_status(resolver.AddQuantize(), "wake AddQuantize");
}

bool ensure_wake_ops()
{
    if (s_wake_resolver_ready) {
        return true;
    }
    s_wake_resolver_ready = register_wake_ops(s_wake_resolver);
    return s_wake_resolver_ready;
}

void reset_buffers()
{
    s_feature_history_frames = 0;
    s_score_window_count = 0;
    s_score_window_index = 0;
    s_last_score = 0.0f;
    s_last_average = 0.0f;
    s_last_peak = 0.0f;
    s_last_peak_threshold = 0.0f;
    s_last_rise_score = 0.0f;
    s_last_active_window_count = 0;
    s_last_min_active_windows = 0;
    s_last_raw_score = 0;
    s_last_reject_reason[0] = '\0';
    std::memset(s_feature_history, 0, sizeof(s_feature_history));
    std::memset(s_score_window, 0, sizeof(s_score_window));
    if (s_frontend_ready) {
        FrontendReset(&s_frontend_state);
    }
}

void destroy_wake_interpreter()
{
    if (s_wake_interpreter_constructed && s_wake_interpreter) {
        s_wake_interpreter->~MicroInterpreter();
    }
    s_wake_interpreter = nullptr;
    s_wake_interpreter_constructed = false;
    s_wake_allocator = nullptr;
    s_wake_resources = nullptr;
    s_wake_arena_used = 0;
}

bool configure_wake_model_bytes(const char *id, const char *label, const uint8_t *data, size_t model_size, const char *source_url)
{
    if (!data || model_size == 0) {
        set_last_error("wake model asset missing");
        ESP_LOGE(TAG, "wake model asset missing");
        return false;
    }

    const tflite::Model *model = tflite::GetModel(data);
    if (!model || model->version() != TFLITE_SCHEMA_VERSION) {
        set_last_errorf("wake schema mismatch model=%u", (unsigned)model_size);
        ESP_LOGE(TAG, "wake model schema mismatch id=%s size=%u", id ? id : "", (unsigned)model_size);
        return false;
    }
    if (!ensure_wake_ops() || !ensure_wake_arena()) {
        return false;
    }

    uint8_t *old_custom_model_data = s_active_custom_model_data;

    s_ready = false;
    destroy_wake_interpreter();
    std::memset(s_wake_arena, 0, kWakeArenaSize);
    s_wake_model = model;
    s_wake_allocator = tflite::MicroAllocator::Create(s_wake_arena, kWakeArenaSize);
    if (!s_wake_allocator) {
        set_last_error("wake allocator Create failed");
        ESP_LOGE(TAG, "wake allocator Create failed");
        return false;
    }
    s_wake_resources = tflite::MicroResourceVariables::Create(s_wake_allocator, kWakeResourceVariableCount);
    if (!s_wake_resources) {
        set_last_error("wake resource variables Create failed");
        ESP_LOGE(TAG, "wake resource variables Create failed");
        return false;
    }
    s_wake_interpreter = new (s_wake_interpreter_storage) tflite::MicroInterpreter(
        s_wake_model,
        s_wake_resolver,
        s_wake_allocator,
        s_wake_resources
    );
    s_wake_interpreter_constructed = true;
    if (s_wake_interpreter->AllocateTensors() != kTfLiteOk) {
        set_last_errorf("wake AllocateTensors failed arena=%u model=%u", (unsigned)kWakeArenaSize, (unsigned)model_size);
        ESP_LOGE(TAG, "wake AllocateTensors failed id=%s arena=%u model=%u", id ? id : "", (unsigned)kWakeArenaSize, (unsigned)model_size);
        destroy_wake_interpreter();
        return false;
    }

    s_wake_arena_used = s_wake_interpreter->arena_used_bytes();
    std::snprintf(s_active_wake_word, sizeof(s_active_wake_word), "%s", id && id[0] ? id : "custom_url");
    std::snprintf(s_active_wake_label, sizeof(s_active_wake_label), "%s", label && label[0] ? label : s_active_wake_word);
    if (source_url && source_url[0]) {
        std::snprintf(s_active_model_source, sizeof(s_active_model_source), "%s", "url");
        std::snprintf(s_active_model_url, sizeof(s_active_model_url), "%s", source_url);
        s_active_custom_model_data = const_cast<uint8_t *>(data);
        s_active_custom_model_size = model_size;
        if (old_custom_model_data && old_custom_model_data != data) {
            heap_caps_free(old_custom_model_data);
        }
    } else {
        std::snprintf(s_active_model_source, sizeof(s_active_model_source), "%s", "embedded");
        s_active_model_url[0] = '\0';
        s_active_custom_model_data = nullptr;
        s_active_custom_model_size = 0;
        if (old_custom_model_data) {
            heap_caps_free(old_custom_model_data);
        }
    }
    log_tensor("wake input", s_wake_interpreter->input(0));
    log_tensor("wake output", s_wake_interpreter->output(0));

    reset_buffers();
    s_ready = true;
    set_last_error("ready");
    ESP_LOGI(
        TAG,
        "microWakeWord model active id=%s label=%s source=%s model=%u wake_arena=%u wake_used=%u wake_psram=%d",
        s_active_wake_word,
        s_active_wake_label,
        s_active_model_source,
        (unsigned)model_size,
        (unsigned)kWakeArenaSize,
        (unsigned)s_wake_arena_used,
        s_wake_arena_psram
    );
    return true;
}

bool configure_wake_model(const tater_wake_model_asset_t *asset)
{
    if (!asset || !asset->data || !asset->end || asset->end <= asset->data) {
        set_last_error("wake model asset missing");
        ESP_LOGE(TAG, "wake model asset missing");
        return false;
    }
    return configure_wake_model_bytes(
        asset->id,
        asset->label,
        asset->data,
        static_cast<size_t>(asset->end - asset->data),
        nullptr
    );
}

bool is_custom_wake_word(const char *wake_word)
{
    return wake_word && (
        std::strcmp(wake_word, "custom_url") == 0 ||
        std::strcmp(wake_word, "custom") == 0 ||
        std::strcmp(wake_word, "url") == 0
    );
}

bool is_absolute_url(const char *url)
{
    return url && (
        std::strncmp(url, "http://", 7) == 0 ||
        std::strncmp(url, "https://", 8) == 0
    );
}

bool ends_with(const char *value, const char *suffix)
{
    if (!value || !suffix) {
        return false;
    }
    size_t value_len = std::strlen(value);
    size_t suffix_len = std::strlen(suffix);
    return value_len >= suffix_len && std::strcmp(value + value_len - suffix_len, suffix) == 0;
}

uint8_t *download_alloc(size_t size)
{
    uint8_t *data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) {
        data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    }
    return data;
}

bool http_download_to_memory(const char *url, size_t max_bytes, bool null_terminate, uint8_t **out_data, size_t *out_size)
{
    if (out_data) {
        *out_data = nullptr;
    }
    if (out_size) {
        *out_size = 0;
    }
    if (!url || !url[0] || !out_data || !out_size || max_bytes == 0) {
        return false;
    }

    size_t capacity = max_bytes + (null_terminate ? 1 : 0);
    uint8_t *buffer = download_alloc(capacity);
    if (!buffer) {
        set_last_errorf("wake download alloc failed size=%u", (unsigned)capacity);
        ESP_LOGE(TAG, "wake download alloc failed url=%s size=%u", url, (unsigned)capacity);
        return false;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 4096;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        heap_caps_free(buffer);
        set_last_error("wake download client init failed");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wake download open failed url=%s err=%s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        heap_caps_free(buffer);
        set_last_errorf("wake download open failed: %s", esp_err_to_name(err));
        return false;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    if (content_len > 0 && (uint64_t)content_len > max_bytes) {
        ESP_LOGE(TAG, "wake download too large url=%s bytes=%lld max=%u", url, (long long)content_len, (unsigned)max_bytes);
        esp_http_client_cleanup(client);
        heap_caps_free(buffer);
        set_last_error("wake download too large");
        return false;
    }

    size_t used = 0;
    while (used < max_bytes) {
        int got = esp_http_client_read(client, reinterpret_cast<char *>(buffer + used), max_bytes - used);
        if (got < 0) {
            ESP_LOGE(TAG, "wake download read failed url=%s", url);
            esp_http_client_cleanup(client);
            heap_caps_free(buffer);
            set_last_error("wake download read failed");
            return false;
        }
        if (got == 0) {
            break;
        }
        used += static_cast<size_t>(got);
    }

    esp_http_client_cleanup(client);
    if (used == 0) {
        heap_caps_free(buffer);
        set_last_error("wake download empty");
        ESP_LOGE(TAG, "wake download empty url=%s", url);
        return false;
    }
    if (used >= max_bytes) {
        heap_caps_free(buffer);
        set_last_error("wake download exceeded limit");
        ESP_LOGE(TAG, "wake download exceeded limit url=%s max=%u", url, (unsigned)max_bytes);
        return false;
    }
    if (null_terminate) {
        buffer[used] = 0;
    }
    *out_data = buffer;
    *out_size = used;
    ESP_LOGI(TAG, "wake download complete url=%s bytes=%u", url, (unsigned)used);
    return true;
}

void id_from_url(const char *url, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!url || !url[0]) {
        std::snprintf(out, out_len, "%s", "custom_url");
        return;
    }
    const char *start = std::strrchr(url, '/');
    start = start ? start + 1 : url;
    size_t written = 0;
    for (const char *p = start; *p && *p != '?' && *p != '#' && written + 1 < out_len; ++p) {
        if (*p == '.') {
            break;
        }
        char c = *p == ' ' || *p == '-' ? '_' : *p;
        out[written++] = c;
    }
    out[written] = '\0';
    if (out[0] == '\0') {
        std::snprintf(out, out_len, "%s", "custom_url");
    }
}

void set_capture_last_error(const char *message)
{
    std::snprintf(s_capture_last_error, sizeof(s_capture_last_error), "%s", message ? message : "");
}

uint8_t score_to_raw(float score)
{
    if (score <= 0.0f) {
        return 0;
    }
    if (score >= 1.0f) {
        return 255;
    }
    return static_cast<uint8_t>((score * 255.0f) + 0.5f);
}

const char *wake_environment_name(WakeEnvironment environment)
{
    switch (environment) {
        case WakeEnvironment::FAR_FIELD:
            return "far_field";
        case WakeEnvironment::STRICT:
            return "strict";
        case WakeEnvironment::TV_NEARBY:
            return "tv_nearby";
        case WakeEnvironment::BALANCED:
        default:
            return "balanced";
    }
}

WakeEnvironment wake_environment_from_settings(const tater_live_settings_t *settings)
{
    const char *value = settings ? settings->wake_environment : "";
    if (!value || !value[0]) {
        return WakeEnvironment::BALANCED;
    }
    if (std::strcmp(value, "far_field") == 0 || std::strcmp(value, "quiet") == 0 ||
        std::strcmp(value, "quiet_room") == 0 || std::strcmp(value, "very_sensitive") == 0) {
        return WakeEnvironment::FAR_FIELD;
    }
    if (std::strcmp(value, "strict") == 0) {
        return WakeEnvironment::STRICT;
    }
    if (std::strcmp(value, "tv_nearby") == 0 || std::strcmp(value, "tv") == 0 || std::strcmp(value, "near_tv") == 0) {
        return WakeEnvironment::TV_NEARBY;
    }
    return WakeEnvironment::BALANCED;
}

float peak_threshold_for_environment(WakeEnvironment environment, float threshold)
{
    switch (environment) {
        case WakeEnvironment::STRICT:
            return std::max(threshold, 220.0f / 255.0f);
        case WakeEnvironment::TV_NEARBY:
            return std::max(threshold, 235.0f / 255.0f);
        case WakeEnvironment::FAR_FIELD:
        case WakeEnvironment::BALANCED:
        default:
            return threshold;
    }
}

uint8_t minimum_active_windows_for_environment(WakeEnvironment environment, size_t window)
{
    if (window == 0) {
        return 0;
    }
    size_t minimum = 1;
    switch (environment) {
        case WakeEnvironment::FAR_FIELD:
            minimum = 1;
            break;
        case WakeEnvironment::STRICT:
            minimum = std::max<size_t>(2, ((window * 2) + 2) / 3);
            break;
        case WakeEnvironment::TV_NEARBY:
            minimum = std::max<size_t>(3, ((window * 3) + 3) / 4);
            break;
        case WakeEnvironment::BALANCED:
        default:
            minimum = std::max<size_t>(1, (window + 1) / 2);
            break;
    }
    return static_cast<uint8_t>(std::min(minimum, window));
}

float minimum_rise_score_for_environment(WakeEnvironment environment)
{
    switch (environment) {
        case WakeEnvironment::STRICT:
            return -8.0f / 255.0f;
        case WakeEnvironment::TV_NEARBY:
            return 4.0f / 255.0f;
        case WakeEnvironment::FAR_FIELD:
        case WakeEnvironment::BALANCED:
        default:
            return -1.0f;
    }
}

int64_t cooldown_us_for_environment(WakeEnvironment environment)
{
    switch (environment) {
        case WakeEnvironment::FAR_FIELD:
            return 800000;
        case WakeEnvironment::STRICT:
            return 1600000;
        case WakeEnvironment::TV_NEARBY:
            return 2400000;
        case WakeEnvironment::BALANCED:
        default:
            return 1200000;
    }
}

size_t normalized_score_window(const tater_live_settings_t *settings)
{
    size_t window = settings ? settings->wake_sliding_window : 5;
    if (window < 1) {
        return 1;
    }
    if (window > kMaxSlidingWindow) {
        return kMaxSlidingWindow;
    }
    return window;
}

size_t score_window_count(size_t window)
{
    return std::min(s_score_window_count, window);
}

size_t score_window_oldest_index(size_t count, size_t window)
{
    return count >= window ? s_score_window_index : 0;
}

float score_window_value_at(size_t offset, size_t count, size_t window)
{
    if (count == 0 || window == 0) {
        return 0.0f;
    }
    size_t oldest = score_window_oldest_index(count, window);
    return s_score_window[(oldest + offset) % window];
}

float score_window_rise_score(size_t count, size_t window)
{
    if (count < window || window < 2) {
        return 0.0f;
    }
    size_t edge_count = window / 2;
    if (edge_count == 0) {
        return 0.0f;
    }

    float early_sum = 0.0f;
    float late_sum = 0.0f;
    for (size_t i = 0; i < edge_count; i++) {
        early_sum += score_window_value_at(i, count, window);
        late_sum += score_window_value_at(window - edge_count + i, count, window);
    }
    return (late_sum / static_cast<float>(edge_count)) - (early_sum / static_cast<float>(edge_count));
}

void format_probability_history(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    size_t used = 0;
    size_t window = tater_live_settings_get() ? tater_live_settings_get()->wake_sliding_window : 5;
    if (window < 1) {
        window = 1;
    } else if (window > kMaxSlidingWindow) {
        window = kMaxSlidingWindow;
    }
    size_t count = s_score_window_count > window ? window : s_score_window_count;
    size_t oldest = count >= window ? s_score_window_index : 0;
    for (size_t i = 0; i < count && used + 1 < out_len; i++) {
        size_t index = (oldest + i) % window;
        int written = std::snprintf(
            out + used,
            out_len - used,
            "%s%u",
            used == 0 ? "" : ",",
            (unsigned)score_to_raw(s_score_window[index])
        );
        if (written <= 0) {
            break;
        }
        if ((size_t)written >= out_len - used) {
            out[out_len - 1] = '\0';
            break;
        }
        used += (size_t)written;
    }
}

uint8_t active_windows_at_or_above(float cutoff, size_t window)
{
    uint8_t active = 0;
    size_t count = score_window_count(window);
    for (size_t i = 0; i < count; i++) {
        if (score_window_value_at(i, count, window) >= cutoff) {
            active++;
        }
    }
    return active;
}

bool ensure_capture_ring()
{
    if (s_capture_ring) {
        return true;
    }
    s_capture_ring = static_cast<int16_t *>(
        heap_caps_malloc(kCaptureBufferSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
    s_capture_ring_psram = s_capture_ring != nullptr;
    if (!s_capture_ring) {
        s_capture_ring = static_cast<int16_t *>(heap_caps_malloc(kCaptureBufferSamples * sizeof(int16_t), MALLOC_CAP_8BIT));
    }
    if (!s_capture_ring) {
        set_capture_last_error("capture ring alloc failed");
        ESP_LOGW(TAG, "trainer capture ring alloc failed samples=%u", (unsigned)kCaptureBufferSamples);
        return false;
    }
    s_capture_ring_start = 0;
    s_capture_ring_count = 0;
    std::memset(s_capture_ring, 0, kCaptureBufferSamples * sizeof(int16_t));
    ESP_LOGI(TAG, "trainer capture ring ready samples=%u psram=%d", (unsigned)kCaptureBufferSamples, s_capture_ring_psram);
    return true;
}

bool capture_settings_enabled(const tater_live_settings_t *settings)
{
    return settings && (settings->capture_wake_audio || settings->capture_close_misses);
}

void append_capture_audio(const int16_t *pcm, size_t sample_count)
{
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (!capture_settings_enabled(settings) || !pcm || sample_count == 0 || !ensure_capture_ring()) {
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        if (s_capture_ring_count < kCaptureBufferSamples) {
            size_t index = (s_capture_ring_start + s_capture_ring_count) % kCaptureBufferSamples;
            s_capture_ring[index] = pcm[i];
            s_capture_ring_count++;
        } else {
            s_capture_ring[s_capture_ring_start] = pcm[i];
            s_capture_ring_start = (s_capture_ring_start + 1) % kCaptureBufferSamples;
        }
    }
}

uint8_t *snapshot_capture_audio(size_t *out_byte_count)
{
    if (out_byte_count) {
        *out_byte_count = 0;
    }
    if (!out_byte_count || !s_capture_ring || s_capture_ring_count < (size_t)(kAudioSampleRate / 2)) {
        return nullptr;
    }
    size_t sample_count = s_capture_ring_count;
    size_t byte_count = sample_count * sizeof(int16_t);
    uint8_t *raw = static_cast<uint8_t *>(heap_caps_malloc(byte_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!raw) {
        raw = static_cast<uint8_t *>(heap_caps_malloc(byte_count, MALLOC_CAP_8BIT));
    }
    if (!raw) {
        set_capture_last_error("capture snapshot alloc failed");
        ESP_LOGW(TAG, "trainer capture snapshot alloc failed bytes=%u", (unsigned)byte_count);
        return nullptr;
    }
    int16_t *dst = reinterpret_cast<int16_t *>(raw);
    for (size_t i = 0; i < sample_count; i++) {
        dst[i] = s_capture_ring[(s_capture_ring_start + i) % kCaptureBufferSamples];
    }
    *out_byte_count = byte_count;
    return raw;
}

bool tater_server_host(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    const char *server_url = tater_protocol_server_url();
    if (!server_url || !server_url[0]) {
        return false;
    }
    const char *host = std::strstr(server_url, "://");
    host = host ? host + 3 : server_url;
    while (*host == '/') {
        host++;
    }
    size_t used = 0;
    for (const char *p = host; *p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && used + 1 < out_len; ++p) {
        out[used++] = *p;
    }
    out[used] = '\0';
    return used > 0;
}

void resolve_trainer_base_url(const char *base_url, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!base_url || !base_url[0]) {
        return;
    }
    const char *marker = std::strstr(base_url, "://trainer.local");
    if (!marker) {
        std::snprintf(out, out_len, "%s", base_url);
        return;
    }
    char host[96];
    if (!tater_server_host(host, sizeof(host))) {
        std::snprintf(out, out_len, "%s", base_url);
        return;
    }
    const char *after_host = marker + std::strlen("://trainer.local");
    size_t prefix_len = (size_t)((marker + 3) - base_url);
    if (prefix_len >= out_len) {
        std::snprintf(out, out_len, "%s", base_url);
        return;
    }
    std::memcpy(out, base_url, prefix_len);
    out[prefix_len] = '\0';
    size_t used = std::strlen(out);
    std::snprintf(out + used, out_len - used, "%s%s", host, after_host);
}

void build_capture_upload_url(const char *base_url, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!base_url || !base_url[0]) {
        return;
    }
    char resolved_base[256];
    resolve_trainer_base_url(base_url, resolved_base, sizeof(resolved_base));
    const char *base = resolved_base[0] ? resolved_base : base_url;
    if (std::strstr(base, kCaptureUploadPath) != nullptr) {
        std::snprintf(out, out_len, "%s", base);
        return;
    }
    size_t len = std::strlen(base);
    std::snprintf(out, out_len, "%s%s%s", base, len > 0 && base[len - 1] == '/' ? "" : "/", "api/upload_captured_audio_raw");
}

void capture_upload_task(void *arg)
{
    CaptureUploadRequest *request = static_cast<CaptureUploadRequest *>(arg);
    if (!request) {
        vTaskDelete(nullptr);
        return;
    }

    bool ok = false;
    char sample_rate[16] = {0};
    char max_probability[24] = {0};
    char average_probability[24] = {0};
    char probability_cutoff[16] = {0};
    char peak_probability_cutoff[16] = {0};
    char rise_score[24] = {0};
    char active_windows[8] = {0};
    char min_active_windows[8] = {0};
    esp_err_t err = ESP_OK;
    size_t written_total = 0;
    int status_code = 0;
    esp_http_client_config_t cfg = {};
    cfg.url = request->upload_url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kCaptureUploadTimeoutMs;
    cfg.buffer_size = kCaptureUploadBufferSize;
    cfg.buffer_size_tx = kCaptureUploadBufferSize;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        set_capture_last_error("upload client init failed");
        goto done;
    }

    std::snprintf(sample_rate, sizeof(sample_rate), "%d", kAudioSampleRate);
    std::snprintf(max_probability, sizeof(max_probability), "%.6f", (double)request->max_probability);
    std::snprintf(average_probability, sizeof(average_probability), "%.6f", (double)request->average_probability);
    std::snprintf(probability_cutoff, sizeof(probability_cutoff), "%u", (unsigned)score_to_raw(request->probability_cutoff));
    std::snprintf(peak_probability_cutoff, sizeof(peak_probability_cutoff), "%u", (unsigned)score_to_raw(request->peak_probability_cutoff));
    std::snprintf(rise_score, sizeof(rise_score), "%.6f", (double)request->rise_score);
    std::snprintf(active_windows, sizeof(active_windows), "%u", (unsigned)request->active_window_count);
    std::snprintf(min_active_windows, sizeof(min_active_windows), "%u", (unsigned)request->min_active_windows);

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Audio-Format", "pcm_s16le");
    esp_http_client_set_header(client, "X-Sample-Rate", sample_rate);
    esp_http_client_set_header(client, "X-Original-Name", "native_wake_capture.raw");
    esp_http_client_set_header(client, "X-Source-Device", request->source_device);
    esp_http_client_set_header(client, "X-Wake-Word", request->wake_word);
    esp_http_client_set_header(client, "X-Event-Type", request->event_type);
    esp_http_client_set_header(client, "X-Blocked-By-Vad", "false");
    esp_http_client_set_header(client, "X-Max-Probability", max_probability);
    esp_http_client_set_header(client, "X-Average-Probability", average_probability);
    esp_http_client_set_header(client, "X-Probability-Cutoff", probability_cutoff);
    esp_http_client_set_header(client, "X-Peak-Probability-Cutoff", peak_probability_cutoff);
    esp_http_client_set_header(client, "X-Active-Windows", active_windows);
    esp_http_client_set_header(client, "X-Min-Active-Windows", min_active_windows);
    esp_http_client_set_header(client, "X-Detection-Profile", request->detection_profile);
    esp_http_client_set_header(client, "X-Rise-Score", rise_score);
    esp_http_client_set_header(client, "X-Probability-History", request->probability_history);
    esp_http_client_set_header(client, "X-Notes", "tater native satellite");

    err = esp_http_client_open(client, request->byte_count);
    if (err != ESP_OK) {
        set_capture_last_error("upload open failed");
        ESP_LOGW(TAG, "trainer capture upload open failed url=%s err=%s", request->upload_url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        goto done;
    }

    while (written_total < request->byte_count) {
        size_t chunk = std::min(kCaptureUploadBufferSize, request->byte_count - written_total);
        int written = esp_http_client_write(client, reinterpret_cast<const char *>(request->pcm_data + written_total), chunk);
        if (written <= 0) {
            set_capture_last_error("upload write failed");
            ESP_LOGW(TAG, "trainer capture upload write failed event=%s", request->event_type);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            goto done;
        }
        written_total += (size_t)written;
        vTaskDelay(1);
    }

    (void)esp_http_client_fetch_headers(client);
    status_code = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (status_code < 200 || status_code >= 300) {
        set_capture_last_error("upload http error");
        ESP_LOGW(TAG, "trainer capture upload failed status=%d event=%s", status_code, request->event_type);
        goto done;
    }

    ok = true;
    s_capture_upload_count++;
    std::snprintf(s_capture_last_event_type, sizeof(s_capture_last_event_type), "%s", request->event_type);
    set_capture_last_error("");
    ESP_LOGI(
        TAG,
        "trainer capture uploaded event=%s wake=%s bytes=%u avg=%.3f",
        request->event_type,
        request->wake_word,
        (unsigned)request->byte_count,
        (double)request->average_probability
    );
    tater_protocol_send_log("info", "trainer capture uploaded");

done:
    if (!ok) {
        s_capture_upload_failures++;
        tater_protocol_send_log("warn", "trainer capture upload failed");
    }
    if (request->pcm_data) {
        heap_caps_free(request->pcm_data);
    }
    heap_caps_free(request);
    s_capture_upload_running = false;
    vTaskDelete(nullptr);
}

void queue_capture_upload(
    const char *event_type,
    const char *wake_word,
    float max_probability,
    float average_probability,
    float probability_cutoff,
    uint8_t active_window_count,
    uint8_t min_active_windows,
    const char *detection_profile,
    float peak_probability_cutoff,
    float rise_score,
    const char *probability_history
)
{
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (!settings || !event_type || !event_type[0]) {
        return;
    }
    if (std::strcmp(event_type, "wake_detected") == 0 && !settings->capture_wake_audio) {
        return;
    }
    if (std::strcmp(event_type, "wake_detected") != 0 && !settings->capture_close_misses) {
        return;
    }
    if (s_capture_upload_running) {
        ESP_LOGW(TAG, "trainer capture upload already running; skipped event=%s", event_type);
        return;
    }

    char upload_url[256];
    build_capture_upload_url(settings->trainer_app_url, upload_url, sizeof(upload_url));
    if (!upload_url[0]) {
        set_capture_last_error("trainer url missing");
        ESP_LOGW(TAG, "trainer capture skipped: trainer URL missing");
        return;
    }

    size_t byte_count = 0;
    uint8_t *pcm_data = snapshot_capture_audio(&byte_count);
    if (!pcm_data || byte_count == 0) {
        set_capture_last_error("capture buffer empty");
        ESP_LOGW(TAG, "trainer capture skipped: capture buffer empty");
        return;
    }

    CaptureUploadRequest *request = static_cast<CaptureUploadRequest *>(
        heap_caps_calloc(1, sizeof(CaptureUploadRequest), MALLOC_CAP_8BIT)
    );
    if (!request) {
        heap_caps_free(pcm_data);
        set_capture_last_error("upload request alloc failed");
        ESP_LOGW(TAG, "trainer capture request alloc failed");
        return;
    }

    request->pcm_data = pcm_data;
    request->byte_count = byte_count;
    std::snprintf(request->upload_url, sizeof(request->upload_url), "%s", upload_url);
    std::snprintf(request->source_device, sizeof(request->source_device), "%s", tater_protocol_device_name());
    std::snprintf(request->wake_word, sizeof(request->wake_word), "%s", wake_word && wake_word[0] ? wake_word : s_active_wake_word);
    std::snprintf(request->event_type, sizeof(request->event_type), "%s", event_type);
    std::snprintf(
        request->detection_profile,
        sizeof(request->detection_profile),
        "%s",
        detection_profile && detection_profile[0] ? detection_profile : s_last_detection_profile
    );
    std::snprintf(
        request->probability_history,
        sizeof(request->probability_history),
        "%s",
        probability_history ? probability_history : ""
    );
    request->max_probability = max_probability;
    request->average_probability = average_probability;
    request->probability_cutoff = probability_cutoff;
    request->peak_probability_cutoff = peak_probability_cutoff > 0.0f ? peak_probability_cutoff : probability_cutoff;
    request->rise_score = rise_score;
    request->active_window_count = active_window_count;
    request->min_active_windows = min_active_windows;

    s_capture_upload_running = true;
    BaseType_t created = xTaskCreatePinnedToCore(capture_upload_task, "wake_capture_up", 8192, request, 3, NULL, 0);
    if (created != pdPASS) {
        s_capture_upload_running = false;
        heap_caps_free(request->pcm_data);
        heap_caps_free(request);
        set_capture_last_error("upload task create failed");
        ESP_LOGW(TAG, "trainer capture upload task create failed");
    }
}

void clear_pending_close_miss()
{
    s_pending_close_miss = {};
}

void flush_pending_close_miss()
{
    if (!s_pending_close_miss.active) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    if (now_us < s_pending_close_miss.due_us) {
        return;
    }
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (!settings || !settings->capture_close_misses ||
        (s_last_close_miss_upload_us > 0 && (now_us - s_last_close_miss_upload_us) < kCloseMissUploadCooldownUs)) {
        clear_pending_close_miss();
        return;
    }
    PendingCloseMiss event = s_pending_close_miss;
    clear_pending_close_miss();
    s_last_close_miss_upload_us = now_us;
    queue_capture_upload(
        "close_miss",
        event.wake_word,
        event.max_probability,
        event.average_probability,
        event.probability_cutoff,
        event.active_window_count,
        event.min_active_windows,
        event.detection_profile,
        event.peak_probability_cutoff,
        event.rise_score,
        event.probability_history
    );
}

void maybe_queue_close_miss(
    const tater_live_settings_t *settings,
    const char *wake_word,
    float score,
    float average,
    float probability_cutoff,
    uint8_t active_window_count,
    uint8_t min_active_windows,
    const char *detection_profile,
    float peak_probability_cutoff,
    float rise_score,
    const char *probability_history
)
{
    if (!settings || !settings->capture_close_misses || average < settings->close_miss_threshold || average >= probability_cutoff) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    if (s_last_close_miss_upload_us > 0 && (now_us - s_last_close_miss_upload_us) < kCloseMissUploadCooldownUs) {
        return;
    }
    if (s_pending_close_miss.active && average <= s_pending_close_miss.average_probability) {
        return;
    }
    s_pending_close_miss.active = true;
    if (s_pending_close_miss.due_us <= 0 || now_us >= s_pending_close_miss.due_us) {
        s_pending_close_miss.due_us = now_us + kCloseMissConfirmationDelayUs;
    }
    std::snprintf(s_pending_close_miss.wake_word, sizeof(s_pending_close_miss.wake_word), "%s", wake_word ? wake_word : "");
    std::snprintf(
        s_pending_close_miss.detection_profile,
        sizeof(s_pending_close_miss.detection_profile),
        "%s",
        detection_profile && detection_profile[0] ? detection_profile : s_last_detection_profile
    );
    std::snprintf(
        s_pending_close_miss.probability_history,
        sizeof(s_pending_close_miss.probability_history),
        "%s",
        probability_history ? probability_history : ""
    );
    s_pending_close_miss.max_probability = score;
    s_pending_close_miss.average_probability = average;
    s_pending_close_miss.probability_cutoff = probability_cutoff;
    s_pending_close_miss.peak_probability_cutoff = peak_probability_cutoff;
    s_pending_close_miss.rise_score = rise_score;
    s_pending_close_miss.active_window_count = active_window_count;
    s_pending_close_miss.min_active_windows = min_active_windows;
}

bool resolve_model_url(const char *json_url, const char *model_ref, char *out, size_t out_len)
{
    if (!json_url || !model_ref || !model_ref[0] || !out || out_len == 0) {
        return false;
    }
    if (is_absolute_url(model_ref)) {
        std::snprintf(out, out_len, "%s", model_ref);
        return true;
    }

    char clean_base[256];
    std::snprintf(clean_base, sizeof(clean_base), "%s", json_url);
    for (char *p = clean_base; *p; ++p) {
        if (*p == '?' || *p == '#') {
            *p = '\0';
            break;
        }
    }

    if (model_ref[0] == '/') {
        const char *scheme = std::strstr(clean_base, "://");
        if (!scheme) {
            return false;
        }
        const char *host_start = scheme + 3;
        const char *host_end = std::strchr(host_start, '/');
        if (!host_end) {
            std::snprintf(out, out_len, "%s%s", clean_base, model_ref);
            return true;
        }
        size_t origin_len = static_cast<size_t>(host_end - clean_base);
        if (origin_len + std::strlen(model_ref) + 1 > out_len) {
            return false;
        }
        std::memcpy(out, clean_base, origin_len);
        out[origin_len] = '\0';
        std::strncat(out, model_ref, out_len - std::strlen(out) - 1);
        return true;
    }

    char *last_slash = std::strrchr(clean_base, '/');
    if (!last_slash) {
        return false;
    }
    *(last_slash + 1) = '\0';
    if (std::strlen(clean_base) + std::strlen(model_ref) + 1 > out_len) {
        return false;
    }
    std::snprintf(out, out_len, "%s%s", clean_base, model_ref);
    return true;
}

bool parse_wake_model_json(const char *json_url, const uint8_t *json_data, size_t json_size, DownloadedWakeModel *model)
{
    if (!json_data || json_size == 0 || !model) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(json_data), json_size);
    if (!root) {
        set_last_error("wake model json parse failed");
        ESP_LOGE(TAG, "wake model json parse failed url=%s", json_url ? json_url : "");
        return false;
    }

    const cJSON *wake_word = cJSON_GetObjectItem(root, "wake_word");
    const cJSON *label = cJSON_GetObjectItem(root, "label");
    const cJSON *model_item = cJSON_GetObjectItem(root, "model");
    if (!cJSON_IsString(model_item) || !model_item->valuestring || !model_item->valuestring[0]) {
        model_item = cJSON_GetObjectItem(root, "model_url");
    }
    if (!cJSON_IsString(model_item) || !model_item->valuestring || !model_item->valuestring[0]) {
        cJSON_Delete(root);
        set_last_error("wake model json missing model");
        ESP_LOGE(TAG, "wake model json missing model url=%s", json_url ? json_url : "");
        return false;
    }

    if (cJSON_IsString(wake_word) && wake_word->valuestring && wake_word->valuestring[0]) {
        std::snprintf(model->id, sizeof(model->id), "%s", wake_word->valuestring);
    } else {
        id_from_url(model_item->valuestring, model->id, sizeof(model->id));
    }
    if (cJSON_IsString(label) && label->valuestring && label->valuestring[0]) {
        std::snprintf(model->label, sizeof(model->label), "%s", label->valuestring);
    } else {
        std::snprintf(model->label, sizeof(model->label), "%s", model->id);
    }
    char model_ref[256];
    std::snprintf(model_ref, sizeof(model_ref), "%s", model_item->valuestring);
    bool resolved = resolve_model_url(json_url, model_ref, model->model_url, sizeof(model->model_url));
    std::snprintf(model->source_url, sizeof(model->source_url), "%s", json_url ? json_url : "");
    cJSON_Delete(root);
    if (!resolved) {
        set_last_error("wake model url resolve failed");
        ESP_LOGE(TAG, "wake model url resolve failed base=%s ref=%s", json_url ? json_url : "", model_ref);
        return false;
    }
    return true;
}

void set_custom_download_running(bool running, const char *url)
{
    if (s_custom_model_lock) {
        xSemaphoreTake(s_custom_model_lock, portMAX_DELAY);
    }
    s_custom_download_running = running;
    std::snprintf(s_downloading_custom_url, sizeof(s_downloading_custom_url), "%s", running && url ? url : "");
    if (s_custom_model_lock) {
        xSemaphoreGive(s_custom_model_lock);
    }
}

void queue_pending_custom_model(DownloadedWakeModel *model)
{
    if (!model || !model->data || model->size == 0) {
        return;
    }
    if (s_custom_model_lock) {
        xSemaphoreTake(s_custom_model_lock, portMAX_DELAY);
    }
    if (s_pending_custom_model.data) {
        heap_caps_free(s_pending_custom_model.data);
    }
    s_pending_custom_model = *model;
    model->data = nullptr;
    model->size = 0;
    if (s_custom_model_lock) {
        xSemaphoreGive(s_custom_model_lock);
    }
}

void wake_model_download_task(void *arg)
{
    WakeDownloadRequest *request = static_cast<WakeDownloadRequest *>(arg);
    char requested_url[sizeof(request->url)] = {0};
    if (request) {
        std::snprintf(requested_url, sizeof(requested_url), "%s", request->url);
        heap_caps_free(request);
    }

    DownloadedWakeModel model = {};
    uint8_t *json_data = nullptr;
    size_t json_size = 0;
    bool ok = false;

    ESP_LOGI(TAG, "wake model custom download starting url=%s", requested_url);
    tater_protocol_send_log("info", "wake model download starting");

    if (ends_with(requested_url, ".tflite")) {
        id_from_url(requested_url, model.id, sizeof(model.id));
        std::snprintf(model.label, sizeof(model.label), "%s", model.id);
        std::snprintf(model.source_url, sizeof(model.source_url), "%s", requested_url);
        std::snprintf(model.model_url, sizeof(model.model_url), "%s", requested_url);
    } else if (http_download_to_memory(requested_url, kWakeManifestMaxBytes, true, &json_data, &json_size)) {
        ok = parse_wake_model_json(requested_url, json_data, json_size, &model);
        heap_caps_free(json_data);
        json_data = nullptr;
        if (!ok) {
            goto done;
        }
    } else {
        goto done;
    }

    ok = http_download_to_memory(model.model_url, kWakeDownloadedModelMaxBytes, false, &model.data, &model.size);
    if (!ok) {
        goto done;
    }
    if (!tflite::GetModel(model.data) || tflite::GetModel(model.data)->version() != TFLITE_SCHEMA_VERSION) {
        set_last_error("downloaded wake schema mismatch");
        ESP_LOGE(TAG, "downloaded wake model schema mismatch url=%s bytes=%u", model.model_url, (unsigned)model.size);
        ok = false;
        goto done;
    }
    queue_pending_custom_model(&model);
    set_last_error("wake model download ready");
    ESP_LOGI(TAG, "wake model custom download ready id=%s source=%s model=%s bytes=%u", model.id, model.source_url, model.model_url, (unsigned)model.size);
    tater_protocol_send_log("info", "wake model download ready");

done:
    if (!ok) {
        s_custom_download_failures++;
        ESP_LOGE(TAG, "wake model custom download failed url=%s", requested_url);
        tater_protocol_send_log("error", "wake model download failed");
    }
    if (json_data) {
        heap_caps_free(json_data);
    }
    if (model.data) {
        heap_caps_free(model.data);
    }
    set_custom_download_running(false, "");
    vTaskDelete(NULL);
}

void consume_pending_custom_model(const tater_live_settings_t *settings)
{
    DownloadedWakeModel pending = {};
    if (s_custom_model_lock) {
        xSemaphoreTake(s_custom_model_lock, portMAX_DELAY);
    }
    if (s_pending_custom_model.data) {
        pending = s_pending_custom_model;
        s_pending_custom_model = {};
    }
    if (s_custom_model_lock) {
        xSemaphoreGive(s_custom_model_lock);
    }
    if (!pending.data) {
        return;
    }

    const char *selected_url = settings ? settings->wake_word_url : "";
    if (!settings || !is_custom_wake_word(settings->wake_word) || std::strcmp(selected_url, pending.source_url) != 0) {
        ESP_LOGW(TAG, "discarding stale custom wake model url=%s selected=%s", pending.source_url, selected_url ? selected_url : "");
        heap_caps_free(pending.data);
        return;
    }

    if (!configure_wake_model_bytes(pending.id, pending.label, pending.data, pending.size, pending.source_url)) {
        heap_caps_free(pending.data);
    }
}

bool start_custom_wake_model_download(const char *url)
{
    if (!url || !url[0]) {
        set_last_error("custom wake url missing");
        return false;
    }
    if (s_ready && std::strcmp(s_active_model_source, "url") == 0 && std::strcmp(s_active_model_url, url) == 0) {
        return true;
    }
    if (s_custom_model_lock) {
        xSemaphoreTake(s_custom_model_lock, portMAX_DELAY);
    }
    bool already_running = s_custom_download_running;
    bool already_requested = std::strcmp(s_requested_custom_url, url) == 0;
    if (already_running || already_requested) {
        if (s_custom_model_lock) {
            xSemaphoreGive(s_custom_model_lock);
        }
        return s_ready;
    }
    std::snprintf(s_requested_custom_url, sizeof(s_requested_custom_url), "%s", url);
    s_custom_download_running = true;
    std::snprintf(s_downloading_custom_url, sizeof(s_downloading_custom_url), "%s", url);
    if (s_custom_model_lock) {
        xSemaphoreGive(s_custom_model_lock);
    }

    WakeDownloadRequest *request = static_cast<WakeDownloadRequest *>(heap_caps_calloc(1, sizeof(WakeDownloadRequest), MALLOC_CAP_8BIT));
    if (!request) {
        set_custom_download_running(false, "");
        set_last_error("custom wake request alloc failed");
        return false;
    }
    std::snprintf(request->url, sizeof(request->url), "%s", url);
    BaseType_t created = xTaskCreatePinnedToCore(wake_model_download_task, "wake_model_dl", 8192, request, 4, NULL, 1);
    if (created != pdPASS) {
        heap_caps_free(request);
        set_custom_download_running(false, "");
        set_last_error("custom wake download task failed");
        return false;
    }
    set_last_error("wake model downloading");
    return s_ready;
}

bool ensure_selected_wake_model(const tater_live_settings_t *settings)
{
    const char *wake_word = settings ? settings->wake_word : "hey_tater";
    if (is_custom_wake_word(wake_word)) {
        return start_custom_wake_model_download(settings ? settings->wake_word_url : "");
    }

    const tater_wake_model_asset_t *asset = tater_wake_model_asset_lookup(wake_word);
    if (!asset) {
        if (std::strcmp(s_pending_wake_word, wake_word ? wake_word : "") != 0) {
            std::snprintf(s_pending_wake_word, sizeof(s_pending_wake_word), "%s", wake_word ? wake_word : "");
            set_last_errorf("wake model unavailable: %s", s_pending_wake_word);
            ESP_LOGW(TAG, "wake model unavailable id=%s", s_pending_wake_word);
            tater_protocol_send_log("warn", "wake model unavailable");
        }
        return s_ready;
    }
    if (s_ready && std::strcmp(s_active_model_source, "embedded") == 0 && std::strcmp(asset->id, s_active_wake_word) == 0) {
        return true;
    }
    std::snprintf(s_pending_wake_word, sizeof(s_pending_wake_word), "%s", asset->id);
    return configure_wake_model(asset);
}

bool runtime_enabled()
{
    if (!s_ready) {
        return false;
    }
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (!settings || std::strcmp(settings->wake_engine, "micro_wake_word") != 0) {
        return false;
    }
    if (esp_timer_get_time() < s_cooldown_until_us) {
        return false;
    }
    return tater_protocol_can_start_local_wake() &&
        !tater_playback_is_playing() &&
        !tater_ota_is_running();
}

bool debug_logging_enabled(const tater_live_settings_t *settings)
{
    return settings && std::strcmp(settings->logging_level, "debug") == 0;
}

const char *tensor_type_name(TfLiteType type)
{
    switch (type) {
        case kTfLiteFloat32:
            return "float32";
        case kTfLiteInt32:
            return "int32";
        case kTfLiteUInt8:
            return "uint8";
        case kTfLiteInt64:
            return "int64";
        case kTfLiteString:
            return "string";
        case kTfLiteBool:
            return "bool";
        case kTfLiteInt16:
            return "int16";
        case kTfLiteComplex64:
            return "complex64";
        case kTfLiteInt8:
            return "int8";
        case kTfLiteFloat16:
            return "float16";
        case kTfLiteFloat64:
            return "float64";
        case kTfLiteComplex128:
            return "complex128";
        case kTfLiteUInt64:
            return "uint64";
        case kTfLiteResource:
            return "resource";
        case kTfLiteVariant:
            return "variant";
        case kTfLiteUInt32:
            return "uint32";
        case kTfLiteUInt16:
            return "uint16";
        case kTfLiteInt4:
            return "int4";
        default:
            return "unknown";
    }
}

void format_tensor_dims(const TfLiteTensor *tensor, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    if (!tensor || !tensor->dims) {
        std::snprintf(buffer, buffer_size, "[]");
        return;
    }
    size_t used = 0;
    used += std::snprintf(buffer + used, buffer_size - used, "[");
    for (int i = 0; i < tensor->dims->size && used < buffer_size; i++) {
        used += std::snprintf(
            buffer + used,
            buffer_size - used,
            "%s%d",
            i == 0 ? "" : ",",
            tensor->dims->data[i]
        );
    }
    if (used < buffer_size) {
        std::snprintf(buffer + used, buffer_size - used, "]");
    } else {
        buffer[buffer_size - 1] = '\0';
    }
}

void log_tensor(const char *label, const TfLiteTensor *tensor)
{
    char dims[64];
    format_tensor_dims(tensor, dims, sizeof(dims));
    if (!tensor) {
        ESP_LOGW(TAG, "%s tensor missing", label);
        return;
    }
    ESP_LOGI(
        TAG,
        "%s tensor type=%s bytes=%u dims=%s scale=%.8f zero_point=%ld",
        label,
        tensor_type_name(tensor->type),
        (unsigned)tensor->bytes,
        dims,
        (double)tensor->params.scale,
        (long)tensor->params.zero_point
    );
}

bool convert_frontend_output(const FrontendOutput &frontend_output, int8_t *feature_out)
{
    if (!feature_out || !frontend_output.values || frontend_output.size != kFeatureSize) {
        ESP_LOGW(TAG, "frontend output mismatch size=%u", (unsigned)frontend_output.size);
        return false;
    }

    int32_t feature_min = INT8_MAX;
    int32_t feature_max = INT8_MIN;
    int32_t feature_sum = 0;
    for (int i = 0; i < kFeatureSize; i++) {
        int32_t value = ((frontend_output.values[i] * kFrontendValueScale) + (kFrontendValueDiv / 2)) / kFrontendValueDiv;
        value += INT8_MIN;
        value = std::min<int32_t>(std::max<int32_t>(value, INT8_MIN), INT8_MAX);
        feature_out[i] = static_cast<int8_t>(value);
        if (value < feature_min) {
            feature_min = value;
        }
        if (value > feature_max) {
            feature_max = value;
        }
        feature_sum += value;
    }
    s_feature_last_min = feature_min;
    s_feature_last_max = feature_max;
    s_feature_last_mean = static_cast<float>(feature_sum) / static_cast<float>(kFeatureSize);
    s_feature_frames++;
    return true;
}

float run_wake_model()
{
    TfLiteTensor *input = s_wake_interpreter ? s_wake_interpreter->input(0) : nullptr;
    TfLiteTensor *output = s_wake_interpreter ? s_wake_interpreter->output(0) : nullptr;
    if (!input || !output || input->type != kTfLiteInt8 || output->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "wake tensor shape/type mismatch");
        return 0.0f;
    }
    if (input->bytes < kWakeInputElements || output->bytes < 1) {
        ESP_LOGE(TAG, "wake tensor too small input=%u output=%u", (unsigned)input->bytes, (unsigned)output->bytes);
        return 0.0f;
    }

    std::memcpy(input->data.int8, s_feature_history, kWakeInputElements);
    if (s_wake_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "wake model invoke failed");
        return 0.0f;
    }

    s_inference_count++;
    uint8_t raw = output->data.uint8[0];
    s_last_raw_score = raw;
    float score = static_cast<float>(raw) / 255.0f;
    if (score < 0.0f) {
        score = 0.0f;
    } else if (score > 1.0f) {
        score = 1.0f;
    }
    return score;
}

bool score_window_triggered(float score)
{
    const tater_live_settings_t *settings = tater_live_settings_get();
    size_t window = normalized_score_window(settings);
    float threshold = settings ? settings->wake_threshold : 0.97f;
    WakeEnvironment environment = wake_environment_from_settings(settings);
    const char *profile = wake_environment_name(environment);
    std::snprintf(s_last_detection_profile, sizeof(s_last_detection_profile), "%s", profile);

    s_score_window[s_score_window_index] = score;
    s_score_window_index = (s_score_window_index + 1) % window;
    if (s_score_window_count < window) {
        s_score_window_count++;
    }

    size_t count = score_window_count(window);
    float sum = 0.0f;
    float peak = 0.0f;
    for (size_t i = 0; i < count; i++) {
        float value = score_window_value_at(i, count, window);
        sum += value;
        if (value > peak) {
            peak = value;
        }
    }
    s_last_average = count > 0 ? sum / static_cast<float>(count) : 0.0f;
    s_last_peak = peak;
    s_last_peak_threshold = peak_threshold_for_environment(environment, threshold);
    s_last_active_window_count = active_windows_at_or_above(threshold, window);
    s_last_min_active_windows = minimum_active_windows_for_environment(environment, window);
    s_last_rise_score = score_window_rise_score(count, window);

    if (count < window) {
        std::snprintf(s_last_reject_reason, sizeof(s_last_reject_reason), "%s", "warming_up");
        return false;
    }

    bool average_detected = s_last_average >= threshold;
    bool peak_detected = s_last_peak >= s_last_peak_threshold;
    bool active_windows_detected = s_last_active_window_count >= s_last_min_active_windows;
    bool shape_detected = s_last_rise_score >= minimum_rise_score_for_environment(environment);
    bool detected = average_detected && peak_detected && active_windows_detected && shape_detected;
    if (detected) {
        s_last_reject_reason[0] = '\0';
    } else if (!average_detected) {
        std::snprintf(s_last_reject_reason, sizeof(s_last_reject_reason), "%s", "average");
    } else if (!peak_detected) {
        std::snprintf(s_last_reject_reason, sizeof(s_last_reject_reason), "%s", "peak");
    } else if (!active_windows_detected) {
        std::snprintf(s_last_reject_reason, sizeof(s_last_reject_reason), "%s", "active_windows");
    } else {
        std::snprintf(s_last_reject_reason, sizeof(s_last_reject_reason), "%s", "score_shape");
    }
    return detected;
}

void handle_feature_frame(const int8_t *feature)
{
    flush_pending_close_miss();

    size_t slot = s_feature_history_frames % kWakeInputFeatureFrames;
    std::memcpy(s_feature_history + (slot * kFeatureSize), feature, kFeatureSize);
    s_feature_history_frames++;
    if ((s_feature_history_frames % kWakeInputFeatureFrames) != 0) {
        return;
    }

    s_last_score = run_wake_model();
    bool triggered = score_window_triggered(s_last_score);
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (debug_logging_enabled(settings)) {
        int64_t now_us = esp_timer_get_time();
        float threshold = settings ? settings->wake_threshold : 0.97f;
        if ((now_us - s_last_debug_log_us) >= 500000 || s_last_score >= (threshold * 0.5f)) {
            ESP_LOGI(
                TAG,
                "wake score raw=%ld score=%.3f avg=%.3f peak=%.3f threshold=%.3f peak_threshold=%.3f profile=%s active=%u/%u rise=%.3f reject=%s rms=%.1f mean=%.1f audio_peak=%ld feature_min=%ld feature_max=%ld feature_mean=%.1f chunks=%lu",
                (long)s_last_raw_score,
                (double)s_last_score,
                (double)s_last_average,
                (double)s_last_peak,
                (double)threshold,
                (double)s_last_peak_threshold,
                s_last_detection_profile,
                (unsigned)s_last_active_window_count,
                (unsigned)s_last_min_active_windows,
                (double)s_last_rise_score,
                s_last_reject_reason[0] ? s_last_reject_reason : "-",
                (double)s_audio_last_rms,
                (double)s_audio_last_abs_mean,
                (long)s_audio_last_peak,
                (long)s_feature_last_min,
                (long)s_feature_last_max,
                (double)s_feature_last_mean,
                (unsigned long)s_audio_chunks_seen
            );
            s_last_debug_log_us = now_us;
        }
    }
    if (!triggered) {
        const char *wake_word = settings ? settings->wake_word : "hey_tater";
        float threshold = settings ? settings->wake_threshold : 0.97f;
        char probability_history[128];
        format_probability_history(probability_history, sizeof(probability_history));
        maybe_queue_close_miss(
            settings,
            wake_word,
            s_last_score,
            s_last_average,
            threshold,
            active_windows_at_or_above(settings ? settings->close_miss_threshold : 0.78f, normalized_score_window(settings)),
            s_last_min_active_windows,
            s_last_detection_profile,
            s_last_peak_threshold,
            s_last_rise_score,
            probability_history
        );
        return;
    }

    const char *wake_word = settings ? settings->wake_word : "hey_tater";
    float threshold = settings ? settings->wake_threshold : 0.97f;
    char probability_history[128];
    format_probability_history(probability_history, sizeof(probability_history));
    clear_pending_close_miss();
    s_detection_count++;
    s_cooldown_until_us = esp_timer_get_time() + cooldown_us_for_environment(wake_environment_from_settings(settings));
    ESP_LOGI(
        TAG,
        "wake detected score=%.3f avg=%.3f peak=%.3f threshold=%.3f profile=%s active=%u/%u rise=%.3f",
        (double)s_last_score,
        (double)s_last_average,
        (double)s_last_peak,
        (double)(settings ? settings->wake_threshold : 0.97f),
        s_last_detection_profile,
        (unsigned)s_last_active_window_count,
        (unsigned)s_last_min_active_windows,
        (double)s_last_rise_score
    );
    tater_protocol_send_log("info", "local wake word detected");
    queue_capture_upload(
        "wake_detected",
        wake_word,
        s_last_score,
        s_last_average,
        threshold,
        s_last_active_window_count,
        s_last_min_active_windows,
        s_last_detection_profile,
        s_last_peak_threshold,
        s_last_rise_score,
        probability_history
    );
    const tater_wake_sound_asset_t *wake_sound = settings && settings->wake_sound_enabled
        ? tater_wake_sound_asset_lookup(settings->wake_sound)
        : nullptr;
    if (wake_sound) {
        esp_err_t sound_err = tater_playback_play_wav_data_local(
            wake_sound->data,
            (size_t)(wake_sound->end - wake_sound->data),
            wake_sound->id);
        if (sound_err != ESP_OK) {
            ESP_LOGW(TAG, "wake sound start failed: %s", esp_err_to_name(sound_err));
            tater_protocol_send_log("warn", "wake sound start failed");
        }
    } else if (settings && settings->wake_sound_enabled && std::strcmp(settings->wake_sound, "no_sound") != 0) {
        ESP_LOGW(TAG, "wake sound asset not found: %s", settings->wake_sound);
        tater_protocol_send_log("warn", "wake sound asset not found");
    }
    tater_protocol_start_voice(wake_word, "micro_wake_word");
    tater_wake_engine_reset();
}

void process_audio_frontend(const int16_t *pcm, size_t sample_count)
{
    size_t offset = 0;
    while (offset < sample_count) {
        size_t processed_samples = 0;
        FrontendOutput frontend_output = FrontendProcessSamples(
            &s_frontend_state,
            pcm + offset,
            sample_count - offset,
            &processed_samples
        );
        if (processed_samples == 0 && frontend_output.size == 0) {
            break;
        }
        offset += processed_samples;

        if (frontend_output.size == 0) {
            continue;
        }
        int8_t feature[kFeatureSize] = {0};
        if (convert_frontend_output(frontend_output, feature)) {
            handle_feature_frame(feature);
        }
    }
}

}  // namespace

extern "C" esp_err_t tater_wake_engine_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!s_custom_model_lock) {
        s_custom_model_lock = xSemaphoreCreateMutex();
    }

    FrontendFillConfigWithDefaults(&s_frontend_config);
    s_frontend_config.window.size_ms = kFeatureDurationMs;
    s_frontend_config.window.step_size_ms = kFeatureStepMs;
    s_frontend_config.filterbank.num_channels = kFeatureSize;
    s_frontend_config.filterbank.lower_band_limit = kFilterbankLowerBandLimit;
    s_frontend_config.filterbank.upper_band_limit = kFilterbankUpperBandLimit;
    s_frontend_config.noise_reduction.smoothing_bits = kNoiseReductionSmoothingBits;
    s_frontend_config.noise_reduction.even_smoothing = kNoiseReductionEvenSmoothing;
    s_frontend_config.noise_reduction.odd_smoothing = kNoiseReductionOddSmoothing;
    s_frontend_config.noise_reduction.min_signal_remaining = kNoiseReductionMinSignalRemaining;
    s_frontend_config.pcan_gain_control.enable_pcan = kPcanGainControlEnablePcan;
    s_frontend_config.pcan_gain_control.strength = kPcanGainControlStrength;
    s_frontend_config.pcan_gain_control.offset = kPcanGainControlOffset;
    s_frontend_config.pcan_gain_control.gain_bits = kPcanGainControlGainBits;
    s_frontend_config.log_scale.enable_log = kLogScaleEnableLog;
    s_frontend_config.log_scale.scale_shift = kLogScaleScaleShift;
    if (!FrontendPopulateState(&s_frontend_config, &s_frontend_state, kAudioSampleRate)) {
        set_last_error("frontend populate failed");
        ESP_LOGE(TAG, "frontend populate failed");
        return ESP_FAIL;
    }
    s_frontend_ready = true;
    ESP_LOGI(
        TAG,
        "microfrontend ready sample_rate=%d window_ms=%d step_ms=%d channels=%d band=%.0f-%.0f",
        kAudioSampleRate,
        kFeatureDurationMs,
        kFeatureStepMs,
        kFeatureSize,
        (double)kFilterbankLowerBandLimit,
        (double)kFilterbankUpperBandLimit
    );

    const tater_wake_model_asset_t *default_model = tater_wake_model_asset_lookup("hey_tater");
    if (!configure_wake_model(default_model)) {
        return ESP_FAIL;
    }
    ESP_LOGI(
        TAG,
        "microWakeWord ready active_model=%s frontend=tflm_microfrontend wake_arena=%u wake_used=%u wake_psram=%d",
        s_active_wake_word,
        (unsigned)kWakeArenaSize,
        (unsigned)s_wake_arena_used,
        s_wake_arena_psram
    );
    return ESP_OK;
}

extern "C" bool tater_wake_engine_ready(void)
{
    return s_ready;
}

extern "C" void tater_wake_engine_reset(void)
{
    reset_buffers();
    if (s_wake_interpreter) {
        (void)s_wake_interpreter->Reset();
    }
}

extern "C" void tater_wake_engine_note_audio(const int16_t *pcm, size_t sample_count)
{
    if (!pcm || sample_count == 0) {
        return;
    }
    append_capture_audio(pcm, sample_count);

    uint64_t sum_squares = 0;
    uint64_t sum_abs = 0;
    int32_t peak = 0;
    int32_t min_sample = INT16_MAX;
    int32_t max_sample = INT16_MIN;

    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = pcm[i];
        int32_t abs_sample = sample < 0 ? (sample == INT16_MIN ? 32768 : -sample) : sample;
        if (abs_sample > peak) {
            peak = abs_sample;
        }
        if (sample < min_sample) {
            min_sample = sample;
        }
        if (sample > max_sample) {
            max_sample = sample;
        }
        sum_abs += (uint32_t)abs_sample;
        sum_squares += (uint64_t)(sample * sample);
    }

    s_audio_samples_seen += sample_count;
    s_audio_chunks_seen++;
    if (peak > 0) {
        s_audio_nonzero_chunks++;
    }
    s_audio_last_rms = static_cast<float>(std::sqrt(static_cast<double>(sum_squares) / static_cast<double>(sample_count)));
    s_audio_last_abs_mean = static_cast<float>(static_cast<double>(sum_abs) / static_cast<double>(sample_count));
    s_audio_last_peak = peak;
    s_audio_last_min = min_sample;
    s_audio_last_max = max_sample;
    s_audio_last_update_us = esp_timer_get_time();
}

extern "C" void tater_wake_engine_process(const int16_t *pcm, size_t sample_count)
{
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (settings) {
        consume_pending_custom_model(settings);
        (void)ensure_selected_wake_model(settings);
    }
    bool enabled = runtime_enabled();
    if (!enabled) {
        if (s_runtime_was_enabled) {
            tater_wake_engine_reset();
        }
        s_runtime_was_enabled = false;
        return;
    }
    s_runtime_was_enabled = true;

    if (!pcm || sample_count == 0) {
        return;
    }

    process_audio_frontend(pcm, sample_count);
}

extern "C" void tater_wake_engine_add_status(cJSON *payload)
{
    if (!cJSON_IsObject(payload)) {
        return;
    }
    cJSON *wake = cJSON_CreateObject();
    cJSON_AddBoolToObject(wake, "ready", s_ready);
    cJSON_AddStringToObject(wake, "active_wake_word", s_active_wake_word);
    cJSON_AddStringToObject(wake, "active_wake_label", s_active_wake_label);
    cJSON_AddStringToObject(wake, "active_model_source", s_active_model_source);
    cJSON_AddStringToObject(wake, "active_model_url", s_active_model_url);
    bool custom_download_running = false;
    char custom_download_url[256] = {0};
    if (s_custom_model_lock) {
        xSemaphoreTake(s_custom_model_lock, portMAX_DELAY);
    }
    custom_download_running = s_custom_download_running;
    std::snprintf(custom_download_url, sizeof(custom_download_url), "%s", s_downloading_custom_url);
    if (s_custom_model_lock) {
        xSemaphoreGive(s_custom_model_lock);
    }
    cJSON_AddBoolToObject(wake, "custom_download_running", custom_download_running);
    cJSON_AddStringToObject(wake, "custom_download_url", custom_download_url);
    cJSON_AddNumberToObject(wake, "custom_download_failures", s_custom_download_failures);
    cJSON_AddStringToObject(wake, "frontend", "tflm_microfrontend");
    cJSON_AddBoolToObject(wake, "frontend_ready", s_frontend_ready);
    cJSON_AddNumberToObject(wake, "feature_frames", s_feature_frames);
    cJSON_AddNumberToObject(wake, "inferences", s_inference_count);
    cJSON_AddNumberToObject(wake, "detections", s_detection_count);
    cJSON_AddNumberToObject(wake, "preprocessor_arena_used", 0);
    cJSON_AddNumberToObject(wake, "wake_arena_used", (double)s_wake_arena_used);
    cJSON_AddBoolToObject(wake, "wake_arena_psram", s_wake_arena_psram);
    cJSON_AddNumberToObject(wake, "last_raw_score", s_last_raw_score);
    cJSON_AddNumberToObject(wake, "last_score", s_last_score);
    cJSON_AddNumberToObject(wake, "last_average", s_last_average);
    cJSON_AddNumberToObject(wake, "last_peak", s_last_peak);
    cJSON_AddNumberToObject(wake, "last_peak_threshold", s_last_peak_threshold);
    cJSON_AddStringToObject(wake, "last_detection_profile", s_last_detection_profile);
    cJSON_AddNumberToObject(wake, "last_active_windows", s_last_active_window_count);
    cJSON_AddNumberToObject(wake, "last_min_active_windows", s_last_min_active_windows);
    cJSON_AddNumberToObject(wake, "last_rise_score", s_last_rise_score);
    cJSON_AddStringToObject(wake, "last_reject_reason", s_last_reject_reason);
    cJSON_AddStringToObject(wake, "last_error", s_last_error);
    cJSON_AddBoolToObject(wake, "runtime_enabled", s_runtime_was_enabled);
    cJSON *capture = cJSON_CreateObject();
    cJSON_AddBoolToObject(capture, "upload_running", s_capture_upload_running);
    cJSON_AddNumberToObject(capture, "uploads", s_capture_upload_count);
    cJSON_AddNumberToObject(capture, "upload_failures", s_capture_upload_failures);
    cJSON_AddStringToObject(capture, "last_event_type", s_capture_last_event_type);
    cJSON_AddStringToObject(capture, "last_error", s_capture_last_error);
    cJSON_AddNumberToObject(capture, "buffer_samples", (double)s_capture_ring_count);
    cJSON_AddNumberToObject(capture, "buffer_seconds", (double)s_capture_ring_count / (double)kAudioSampleRate);
    cJSON_AddBoolToObject(capture, "buffer_psram", s_capture_ring_psram);
    cJSON_AddBoolToObject(capture, "close_miss_pending", s_pending_close_miss.active);
    cJSON_AddItemToObject(wake, "capture", capture);
    cJSON *mic = cJSON_CreateObject();
    cJSON_AddNumberToObject(mic, "samples_seen", (double)s_audio_samples_seen);
    cJSON_AddNumberToObject(mic, "chunks_seen", s_audio_chunks_seen);
    cJSON_AddNumberToObject(mic, "nonzero_chunks", s_audio_nonzero_chunks);
    cJSON_AddNumberToObject(mic, "last_rms", s_audio_last_rms);
    cJSON_AddNumberToObject(mic, "last_abs_mean", s_audio_last_abs_mean);
    cJSON_AddNumberToObject(mic, "last_peak", s_audio_last_peak);
    cJSON_AddNumberToObject(mic, "last_min", s_audio_last_min);
    cJSON_AddNumberToObject(mic, "last_max", s_audio_last_max);
    int64_t age_ms = s_audio_last_update_us > 0 ? (esp_timer_get_time() - s_audio_last_update_us) / 1000 : -1;
    cJSON_AddNumberToObject(mic, "last_age_ms", (double)age_ms);
    cJSON_AddItemToObject(wake, "mic", mic);
    cJSON *feature = cJSON_CreateObject();
    cJSON_AddNumberToObject(feature, "last_min", s_feature_last_min);
    cJSON_AddNumberToObject(feature, "last_max", s_feature_last_max);
    cJSON_AddNumberToObject(feature, "last_mean", s_feature_last_mean);
    cJSON_AddItemToObject(wake, "feature", feature);
    cJSON_AddItemToObject(payload, "wake_engine", wake);
}
