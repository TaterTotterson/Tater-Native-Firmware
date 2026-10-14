#include "sendspin_player.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <strings.h>
#include <utility>
#include <vector>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "sendspin/client.h"
#include "sendspin/color_role.h"
#include "sendspin/player_role.h"
#include "sendspin/visualizer_role.h"

extern "C" {
#include "audio_i2s.h"
#include "leds.h"
#include "native_settings.h"
#include "playback_mix.h"
#include "tater_protocol.h"
#include "wifi_station.h"
}

namespace {

using sendspin::MemoryLocation;
using sendspin::ColorRoleListener;
using sendspin::PlayerRole;
using sendspin::PlayerRoleConfig;
using sendspin::PlayerRoleListener;
using sendspin::SendspinClient;
using sendspin::SendspinClientConfig;
using sendspin::SendspinClientListener;
using sendspin::SendspinCodecFormat;
using sendspin::SendspinGoodbyeReason;
using sendspin::SendspinNetworkProvider;
using sendspin::SendspinPersistenceProvider;
using sendspin::ServerColorStateObject;
using sendspin::ServerVisualizerStreamObject;
using sendspin::VisualizerDataType;
using sendspin::VisualizerRoleConfig;
using sendspin::VisualizerRoleListener;
using sendspin::VisualizerSpectrumConfig;
using sendspin::VisualizerSpectrumScale;

constexpr uint16_t SENDSPIN_PORT = 8928;
constexpr size_t SENDSPIN_AUDIO_BUFFER_BYTES = 1024 * 1024;
constexpr uint16_t SENDSPIN_MIN_BUFFER_MS = 1500;
constexpr uint16_t SENDSPIN_STARTUP_HEADROOM_MS = 120;
constexpr unsigned SENDSPIN_PROTOCOL_TASK_PRIORITY = 9;
constexpr unsigned SENDSPIN_SYNC_TASK_PRIORITY = 10;
constexpr unsigned SENDSPIN_HTTPD_TASK_PRIORITY = 9;
constexpr unsigned SENDSPIN_RENDER_TASK_PRIORITY = 11;
constexpr size_t SENDSPIN_RENDER_TASK_STACK_BYTES = 4096;
constexpr TickType_t SENDSPIN_VOLUME_PUBLISH_SETTLE_TICKS = pdMS_TO_TICKS(75);
constexpr int64_t SENDSPIN_STARVATION_MIN_US = 10000;
constexpr TickType_t SENDSPIN_LOOP_TICKS = pdMS_TO_TICKS(5);
constexpr uint8_t SENDSPIN_DUCK_TARGET_PERCENT = 20;
constexpr uint16_t SENDSPIN_DUCK_ATTACK_MS = 150;
constexpr uint16_t SENDSPIN_DUCK_RELEASE_MS = 350;
constexpr size_t SENDSPIN_OVERLAY_CAPACITY_FRAMES = TATER_SPK_SAMPLE_RATE / 4;
constexpr size_t SENDSPIN_OVERLAY_MIX_FRAMES = 256;
constexpr const char *SENDSPIN_NVS_NAMESPACE = "sendspin";
constexpr const char *SENDSPIN_NVS_OUTPUT_CHANNEL = "channel_mode";

const char *TAG = "tater_sendspin";

std::atomic<bool> s_initialized{false};
std::atomic<bool> s_sendspin_task_stack_psram{false};
std::atomic<uint32_t> s_native_audio_claim_count{0};
std::atomic<bool> s_stream_active{false};
std::atomic<bool> s_speaker_owned{false};
std::atomic<bool> s_disconnect_requested{false};
std::atomic<bool> s_volume_dirty{false};
std::atomic<TickType_t> s_volume_changed_tick{0};
std::atomic<bool> s_applying_server_volume{false};
std::atomic<bool> s_muted{false};
std::atomic<uint8_t> s_output_volume{80};
std::atomic<uint8_t> s_desired_volume{80};
std::atomic<uint8_t> s_output_channel{TATER_SENDSPIN_OUTPUT_STEREO};
std::atomic<uint64_t> s_written_frames{0};
std::atomic<uint32_t> s_streams_started{0};
std::atomic<uint32_t> s_streams_completed{0};
std::atomic<uint32_t> s_audio_write_failures{0};
std::atomic<uint32_t> s_render_clock_failures{0};
std::atomic<uint32_t> s_output_starvations{0};
std::atomic<uint32_t> s_last_starvation_us{0};
std::atomic<uint32_t> s_max_starvation_us{0};
std::atomic<uint32_t> s_max_queued_frames{0};
std::atomic<uint32_t> s_time_sync_updates{0};
std::atomic<uint32_t> s_clock_error_us{0};
std::atomic<uint32_t> s_max_clock_error_us{0};
std::atomic<uint16_t> s_output_delay_ms{0};
std::atomic<int64_t> s_output_empty_since_us{0};
std::atomic<bool> s_received_stream_audio{false};
std::atomic<bool> s_voice_duck_active{false};
std::atomic<bool> s_overlay_active{false};
std::atomic<uint32_t> s_overlay_read_frame{0};
std::atomic<uint32_t> s_overlay_write_frame{0};
std::atomic<uint32_t> s_duck_revision{0};
int16_t *s_overlay_samples = nullptr;
TaskHandle_t s_render_feedback_task_handle = nullptr;

char s_friendly_name[TATER_CFG_DEVICE_NAME_LEN] = {};
char s_client_id[48] = {};

bool native_audio_claimed() {
    return s_native_audio_claim_count.load(std::memory_order_acquire) != 0;
}

bool duck_requested() {
    return s_voice_duck_active.load(std::memory_order_acquire)
        || s_overlay_active.load(std::memory_order_acquire);
}

void publish_duck_change() {
    s_duck_revision.fetch_add(1, std::memory_order_acq_rel);
}

void reset_overlay_ring() {
    s_overlay_read_frame.store(0, std::memory_order_release);
    s_overlay_write_frame.store(0, std::memory_order_release);
}

void stop_overlay_immediately() {
    bool was_active = s_overlay_active.exchange(false, std::memory_order_acq_rel);
    reset_overlay_ring();
    if (was_active) {
        publish_duck_change();
    }
}

size_t read_overlay_frames(int16_t *out, size_t frame_count) {
    if (!out || frame_count == 0
        || !s_overlay_active.load(std::memory_order_acquire)
        || !s_overlay_samples) {
        return 0;
    }

    uint32_t read_frame = s_overlay_read_frame.load(std::memory_order_relaxed);
    uint32_t write_frame = s_overlay_write_frame.load(std::memory_order_acquire);
    size_t available = static_cast<size_t>(write_frame - read_frame);
    size_t wanted = std::min(frame_count, available);
    size_t copied = 0;
    while (copied < wanted) {
        size_t ring_frame = static_cast<size_t>(read_frame) % SENDSPIN_OVERLAY_CAPACITY_FRAMES;
        size_t contiguous = std::min(wanted - copied, SENDSPIN_OVERLAY_CAPACITY_FRAMES - ring_frame);
        std::memcpy(
            out + (copied * TATER_SPK_CHANNELS),
            s_overlay_samples + (ring_frame * TATER_SPK_CHANNELS),
            contiguous * TATER_SPK_CHANNELS * sizeof(int16_t)
        );
        read_frame += static_cast<uint32_t>(contiguous);
        copied += contiguous;
    }
    s_overlay_read_frame.store(read_frame, std::memory_order_release);
    return copied;
}

int16_t saturate_sample(int32_t sample) {
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return static_cast<int16_t>(sample);
}

void update_atomic_max(std::atomic<uint32_t> &target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(
        current,
        value,
        std::memory_order_relaxed,
        std::memory_order_relaxed
    )) {
    }
}

void finish_output_starvation(int64_t now_us) {
    int64_t started_us = s_output_empty_since_us.exchange(0, std::memory_order_acq_rel);
    if (started_us <= 0 || now_us <= started_us) {
        return;
    }
    int64_t duration_us = now_us - started_us;
    if (duration_us < SENDSPIN_STARVATION_MIN_US) {
        return;
    }
    uint32_t bounded_us = static_cast<uint32_t>(
        std::min<int64_t>(duration_us, UINT32_MAX)
    );
    s_output_starvations.fetch_add(1, std::memory_order_relaxed);
    s_last_starvation_us.store(bounded_us, std::memory_order_relaxed);
    update_atomic_max(s_max_starvation_us, bounded_us);
}

const char *output_channel_name(tater_sendspin_output_channel_t channel) {
    switch (channel) {
    case TATER_SENDSPIN_OUTPUT_LEFT:
        return "left";
    case TATER_SENDSPIN_OUTPUT_RIGHT:
        return "right";
    case TATER_SENDSPIN_OUTPUT_MONO:
        return "mono";
    case TATER_SENDSPIN_OUTPUT_STEREO:
    default:
        return "stereo";
    }
}

bool parse_output_channel(const char *mode, tater_sendspin_output_channel_t *channel) {
    if (!mode || !channel) {
        return false;
    }
    if (strcasecmp(mode, "stereo") == 0) {
        *channel = TATER_SENDSPIN_OUTPUT_STEREO;
    } else if (strcasecmp(mode, "left") == 0) {
        *channel = TATER_SENDSPIN_OUTPUT_LEFT;
    } else if (strcasecmp(mode, "right") == 0) {
        *channel = TATER_SENDSPIN_OUTPUT_RIGHT;
    } else if (strcasecmp(mode, "mono") == 0) {
        *channel = TATER_SENDSPIN_OUTPUT_MONO;
    } else {
        return false;
    }
    return true;
}

bool save_output_channel(tater_sendspin_output_channel_t channel) {
    nvs_handle_t handle = 0;
    if (nvs_open(SENDSPIN_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_u8(handle, SENDSPIN_NVS_OUTPUT_CHANNEL, static_cast<uint8_t>(channel));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

tater_sendspin_output_channel_t load_output_channel() {
    nvs_handle_t handle = 0;
    if (nvs_open(SENDSPIN_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return TATER_SENDSPIN_OUTPUT_STEREO;
    }
    uint8_t value = TATER_SENDSPIN_OUTPUT_STEREO;
    esp_err_t err = nvs_get_u8(handle, SENDSPIN_NVS_OUTPUT_CHANNEL, &value);
    nvs_close(handle);
    if (err != ESP_OK || value > TATER_SENDSPIN_OUTPUT_MONO) {
        return TATER_SENDSPIN_OUTPUT_STEREO;
    }
    return static_cast<tater_sendspin_output_channel_t>(value);
}

tater_playback_channel_t playback_channel() {
    switch (s_output_channel.load(std::memory_order_relaxed)) {
    case TATER_SENDSPIN_OUTPUT_LEFT:
        return TATER_PLAYBACK_CHANNEL_LEFT;
    case TATER_SENDSPIN_OUTPUT_RIGHT:
        return TATER_PLAYBACK_CHANNEL_RIGHT;
    case TATER_SENDSPIN_OUTPUT_MONO:
        return TATER_PLAYBACK_CHANNEL_MONO;
    case TATER_SENDSPIN_OUTPUT_STEREO:
    default:
        return TATER_PLAYBACK_CHANNEL_STEREO;
    }
}

class TaterNetworkProvider final : public SendspinNetworkProvider {
public:
    bool is_network_ready() override {
        return tater_wifi_is_connected();
    }
};

class TaterPersistenceProvider final : public SendspinPersistenceProvider {
public:
    std::optional<std::vector<uint8_t>> load_blob(const std::string &key) override {
        nvs_handle_t handle = 0;
        if (nvs_open(SENDSPIN_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
            return std::nullopt;
        }
        size_t length = 0;
        esp_err_t err = nvs_get_blob(handle, key.c_str(), nullptr, &length);
        if (err != ESP_OK) {
            nvs_close(handle);
            return std::nullopt;
        }
        std::vector<uint8_t> value(length);
        err = nvs_get_blob(handle, key.c_str(), value.data(), &length);
        nvs_close(handle);
        if (err != ESP_OK) {
            return std::nullopt;
        }
        value.resize(length);
        return value;
    }

    bool save_blob(const std::string &key, const uint8_t *data, size_t length) override {
        if (!data && length != 0) {
            return false;
        }
        nvs_handle_t handle = 0;
        if (nvs_open(SENDSPIN_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
            return false;
        }
        esp_err_t err = nvs_set_blob(handle, key.c_str(), data, length);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        return err == ESP_OK;
    }
};

class TaterClientListener final : public SendspinClientListener {
public:
    void on_time_sync_updated(float error) override {
        uint32_t error_us = static_cast<uint32_t>(std::min<double>(
            std::abs(static_cast<double>(error)),
            static_cast<double>(UINT32_MAX)
        ));
        s_time_sync_updates.fetch_add(1, std::memory_order_relaxed);
        s_clock_error_us.store(error_us, std::memory_order_relaxed);
        update_atomic_max(s_max_clock_error_us, error_us);
    }

    void on_request_high_performance() override {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    }

    void on_release_high_performance() override {
        // Native voice transport also needs low-latency Wi-Fi, so leave power save off.
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    }
};

#if !TATER_BOARD_S3_BOX
class TaterColorListener final : public ColorRoleListener {
public:
    void on_color(const ServerColorStateObject &palette) override {
        const auto *selected = palette.primary ? &*palette.primary : nullptr;
        if (palette.accent) {
            const auto &accent = *palette.accent;
            auto luminance = [](const auto &color) {
                return static_cast<uint32_t>(color[0]) * 299U
                    + static_cast<uint32_t>(color[1]) * 587U
                    + static_cast<uint32_t>(color[2]) * 114U;
            };
            if (!selected || luminance(accent) > luminance(*selected)) {
                selected = &accent;
            }
        }
        if (!selected) {
            tater_leds_set_music_color(false, 0, 0, 0);
            return;
        }
        tater_leds_set_music_color(true, (*selected)[0], (*selected)[1], (*selected)[2]);
    }

    void on_color_clear() override {
        tater_leds_set_music_color(false, 0, 0, 0);
    }
};

class TaterVisualizerListener final : public VisualizerRoleListener {
public:
    void on_loudness(int64_t client_timestamp, uint16_t loudness) override {
        (void)client_timestamp;
        latest_loudness_.store(loudness, std::memory_order_release);
        tater_leds_set_music_loudness(loudness, false);
    }

    void on_beat(int64_t client_timestamp, bool downbeat) override {
        (void)client_timestamp;
        (void)downbeat;
        tater_leds_set_music_loudness(
            latest_loudness_.load(std::memory_order_acquire),
            true
        );
    }

    void on_peak(int64_t client_timestamp, uint8_t strength) override {
        (void)client_timestamp;
        if (strength > 0) {
            tater_leds_set_music_loudness(
                latest_loudness_.load(std::memory_order_acquire),
                true
            );
        }
    }

    void on_spectrum(int64_t client_timestamp, const std::vector<uint16_t> &bins) override {
        (void)client_timestamp;
        tater_leds_set_music_spectrum(bins.data(), bins.size());
    }

    void on_visualizer_stream_start(const ServerVisualizerStreamObject &stream) override {
        (void)stream;
        latest_loudness_.store(0, std::memory_order_release);
        tater_leds_clear_music_visualizer();
    }

    void on_visualizer_stream_end() override {
        latest_loudness_.store(0, std::memory_order_release);
        tater_leds_clear_music_visualizer();
    }

    void on_visualizer_stream_clear() override {
        latest_loudness_.store(0, std::memory_order_release);
        tater_leds_clear_music_visualizer();
    }

private:
    std::atomic<uint16_t> latest_loudness_{0};
};
#endif

class TaterPlayerListener final : public PlayerRoleListener {
public:
    explicit TaterPlayerListener(PlayerRole *player) : player_(player) {
        tater_playback_mix_init(&duck_mix_, 100);
    }

    size_t on_audio_write(uint8_t *data, size_t length, uint32_t timeout_ms) override {
        (void)timeout_ms;
        if (!data || length == 0 || (length % (TATER_SPK_CHANNELS * sizeof(int16_t))) != 0) {
            return 0;
        }
        if (!s_speaker_owned.load(std::memory_order_acquire)
            || native_audio_claimed()) {
            return 0;
        }

        finish_output_starvation(esp_timer_get_time());

        int16_t *samples = reinterpret_cast<int16_t *>(data);
        size_t sample_count = length / sizeof(int16_t);
        size_t frames = length / (TATER_SPK_CHANNELS * sizeof(int16_t));
        tater_playback_route_channel(playback_channel(), samples, frames);
        uint8_t volume = s_output_volume.load(std::memory_order_relaxed);
        bool muted = s_muted.load(std::memory_order_relaxed);
        if (muted || volume == 0) {
            std::memset(data, 0, length);
        } else if (volume < 100) {
            for (size_t index = 0; index < sample_count; index++) {
                samples[index] = static_cast<int16_t>(
                    (static_cast<int32_t>(samples[index]) * volume) / 100
                );
            }
        }

        refresh_duck_target();
        int16_t overlay[SENDSPIN_OVERLAY_MIX_FRAMES * TATER_SPK_CHANNELS];
        size_t mixed_frames = 0;
        while (mixed_frames < frames) {
            size_t chunk_frames = std::min(
                frames - mixed_frames,
                SENDSPIN_OVERLAY_MIX_FRAMES
            );
            int16_t *music = samples + (mixed_frames * TATER_SPK_CHANNELS);
            size_t overlay_frames = read_overlay_frames(overlay, chunk_frames);
            if (overlay_frames > 0) {
                tater_playback_mix_frames(
                    &duck_mix_,
                    nullptr,
                    music,
                    music,
                    overlay_frames
                );
                for (size_t sample = 0;
                    sample < overlay_frames * TATER_SPK_CHANNELS;
                    sample++) {
                    music[sample] = saturate_sample(
                        static_cast<int32_t>(music[sample])
                        + static_cast<int32_t>(overlay[sample])
                    );
                }
            }
            if (overlay_frames < chunk_frames) {
                int16_t *music_only = music + (overlay_frames * TATER_SPK_CHANNELS);
                tater_playback_mix_frames(
                    &duck_mix_,
                    nullptr,
                    music_only,
                    music_only,
                    chunk_frames - overlay_frames
                );
            }
            mixed_frames += chunk_frames;
        }

        // Reserve the frames before entering the board writer.  A decoded FLAC block is usually
        // about 96 ms long, while the board writer feeds it to the much smaller I2S DMA queue in
        // several blocking writes.  The render-feedback task runs concurrently and can observe
        // completed DMA frames before this call returns.  Publishing the submission only after
        // the call used to make those completions look unrelated to Sendspin audio, so they were
        // discarded and the sync task later made a large, audible correction.
        uint64_t written = s_written_frames.fetch_add(
            static_cast<uint64_t>(frames),
            std::memory_order_acq_rel
        ) + frames;
        uint64_t notified = notified_frames_.load(std::memory_order_acquire);
        uint64_t queued = written > notified ? written - notified : 0;
        update_atomic_max(
            s_max_queued_frames,
            static_cast<uint32_t>(std::min<uint64_t>(queued, UINT32_MAX))
        );

        esp_err_t err = tater_audio_write_speaker_frames(samples, frames);
        if (err != ESP_OK) {
            // The board writers report an error for a short write and Sendspin disconnects the
            // stream immediately.  Undo the reservation so the final render snapshot cannot
            // report frames that were never accepted by the output path.
            s_written_frames.fetch_sub(static_cast<uint64_t>(frames), std::memory_order_acq_rel);
            ESP_LOGE(TAG, "Sendspin I2S write failed: %s", esp_err_to_name(err));
            s_audio_write_failures.fetch_add(1, std::memory_order_relaxed);
            s_disconnect_requested.store(true, std::memory_order_release);
            return 0;
        }
        s_received_stream_audio.store(true, std::memory_order_release);
        return length;
    }

    void on_stream_start() override {
        s_stream_active.store(true, std::memory_order_release);
        s_streams_started.fetch_add(1, std::memory_order_relaxed);
        s_output_empty_since_us.store(0, std::memory_order_release);
        s_received_stream_audio.store(false, std::memory_order_release);
        if (native_audio_claimed()) {
            ESP_LOGW(TAG, "rejecting Sendspin stream while native audio owns the speaker");
            s_disconnect_requested.store(true, std::memory_order_release);
            return;
        }

        const auto &params = player_->get_current_stream_params();
        if (!params.is_complete()
            || *params.sample_rate != TATER_SPK_SAMPLE_RATE
            || *params.channels != TATER_SPK_CHANNELS
            || *params.bit_depth != 16) {
            ESP_LOGE(TAG, "unsupported negotiated Sendspin audio format");
            s_disconnect_requested.store(true, std::memory_order_release);
            return;
        }

        esp_err_t err = tater_audio_speaker_begin();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Sendspin speaker start failed: %s", esp_err_to_name(err));
            s_disconnect_requested.store(true, std::memory_order_release);
            return;
        }

        tater_audio_render_clock_t clock = {};
        if (!tater_audio_speaker_render_clock_snapshot(&clock)) {
            ESP_LOGE(TAG, "Sendspin render clock unavailable");
            s_render_clock_failures.fetch_add(1, std::memory_order_relaxed);
            tater_audio_speaker_end();
            s_disconnect_requested.store(true, std::memory_order_release);
            return;
        }
        last_completed_frames_ = clock.completed_frames;
        pending_prefix_frames_ = clock.submitted_frames - clock.completed_frames;
        notified_frames_.store(0, std::memory_order_release);
        s_written_frames.store(0, std::memory_order_release);
        s_speaker_owned.store(true, std::memory_order_release);
        tater_leds_set_music_active(true);
        ESP_LOGI(
            TAG,
            "Sendspin stream started codec=%d rate=%u channels=%u prefix_frames=%u",
            static_cast<int>(*params.codec),
            static_cast<unsigned>(*params.sample_rate),
            static_cast<unsigned>(*params.channels),
            static_cast<unsigned>(pending_prefix_frames_)
        );
    }

    void on_stream_end() override {
        stop_overlay_immediately();
        tater_leds_set_music_active(false);
        s_stream_active.store(false, std::memory_order_release);
        s_streams_completed.fetch_add(1, std::memory_order_relaxed);
        s_output_empty_since_us.store(0, std::memory_order_release);
        s_received_stream_audio.store(false, std::memory_order_release);
        if (s_speaker_owned.exchange(false, std::memory_order_acq_rel)) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(tater_audio_speaker_end());
        }
        s_written_frames.store(0, std::memory_order_release);
        notified_frames_.store(0, std::memory_order_release);
        ESP_LOGI(TAG, "Sendspin stream ended");
    }

    void on_volume_changed(uint8_t volume) override {
        volume = std::min<uint8_t>(volume, 100);
        s_output_volume.store(volume, std::memory_order_release);
        s_desired_volume.store(volume, std::memory_order_release);
        s_volume_dirty.store(false, std::memory_order_release);
        s_applying_server_volume.store(true, std::memory_order_release);
        tater_live_settings_set_volume_percent(volume);
        s_applying_server_volume.store(false, std::memory_order_release);
    }

    void on_mute_changed(bool muted) override {
        s_muted.store(muted, std::memory_order_release);
    }

    void on_output_delay_changed(uint16_t delay_ms) override {
        s_output_delay_ms.store(delay_ms, std::memory_order_release);
        ESP_LOGI(
            TAG,
            "Sendspin output delay calibration=%u ms",
            static_cast<unsigned>(delay_ms)
        );
    }

    void poll_render_clock() {
        if (!s_speaker_owned.load(std::memory_order_acquire)) {
            return;
        }
        tater_audio_render_clock_t clock = {};
        if (!tater_audio_speaker_render_clock_snapshot(&clock)) {
            s_render_clock_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        uint32_t completed_delta = clock.completed_frames - last_completed_frames_;
        last_completed_frames_ = clock.completed_frames;
        if (completed_delta <= pending_prefix_frames_) {
            pending_prefix_frames_ -= completed_delta;
            return;
        }
        completed_delta -= pending_prefix_frames_;
        pending_prefix_frames_ = 0;

        uint64_t written = s_written_frames.load(std::memory_order_acquire);
        uint64_t notified = notified_frames_.load(std::memory_order_acquire);
        uint64_t outstanding = written > notified ? written - notified : 0;
        if (outstanding == 0 && written > 0
            && s_received_stream_audio.load(std::memory_order_acquire)) {
            int64_t expected = 0;
            (void)s_output_empty_since_us.compare_exchange_strong(
                expected,
                esp_timer_get_time(),
                std::memory_order_acq_rel,
                std::memory_order_relaxed
            );
        }
        uint32_t played_audio_frames = static_cast<uint32_t>(
            std::min<uint64_t>(completed_delta, outstanding)
        );
        if (played_audio_frames == 0) {
            return;
        }
        notified_frames_.fetch_add(played_audio_frames, std::memory_order_acq_rel);
        const int64_t completed_at_us =
            clock.completed_at_us > 0 ? clock.completed_at_us : esp_timer_get_time();
        player_->notify_audio_played(played_audio_frames, completed_at_us);
    }

private:
    void refresh_duck_target() {
        uint32_t revision = s_duck_revision.load(std::memory_order_acquire);
        if (revision == duck_revision_) {
            return;
        }
        bool active = duck_requested();
        uint32_t ramp_frames = static_cast<uint32_t>(
            (static_cast<uint64_t>(TATER_SPK_SAMPLE_RATE)
                * (active ? SENDSPIN_DUCK_ATTACK_MS : SENDSPIN_DUCK_RELEASE_MS))
            / 1000
        );
        tater_playback_mix_set_background(
            &duck_mix_,
            active ? SENDSPIN_DUCK_TARGET_PERCENT : 100,
            ramp_frames
        );
        duck_revision_ = revision;
        ESP_LOGI(
            TAG,
            "Sendspin music duck=%s target=%u%% ramp_ms=%u",
            active ? "on" : "off",
            active ? SENDSPIN_DUCK_TARGET_PERCENT : 100,
            active ? SENDSPIN_DUCK_ATTACK_MS : SENDSPIN_DUCK_RELEASE_MS
        );
    }

    PlayerRole *player_;
    uint32_t last_completed_frames_{0};
    uint32_t pending_prefix_frames_{0};
    std::atomic<uint64_t> notified_frames_{0};
    tater_playback_mix_state_t duck_mix_{};
    uint32_t duck_revision_{UINT32_MAX};
};

void render_feedback_task(void *arg) {
    auto *listener = static_cast<TaterPlayerListener *>(arg);
    while (true) {
        // I2S completion callbacks wake this task only when the DMA playhead advances. This keeps
        // render timing just as prompt as the previous 1 ms poll, without continuously inserting
        // a priority task into FreeRTOS's delayed list while other cores allocate and publish
        // settings state.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        listener->poll_render_clock();
    }
}

bool start_render_feedback_task(TaterPlayerListener *listener) {
    if (!listener) {
        return false;
    }
    s_render_feedback_task_handle = nullptr;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        render_feedback_task,
        "sendspin_render",
        SENDSPIN_RENDER_TASK_STACK_BYTES,
        listener,
        SENDSPIN_RENDER_TASK_PRIORITY,
        &s_render_feedback_task_handle,
        1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (created == pdPASS) {
        ESP_LOGI(
            TAG,
            "Sendspin render feedback task started priority=%u core=1 stack=psram",
            SENDSPIN_RENDER_TASK_PRIORITY
        );
        return true;
    }
    ESP_LOGW(TAG, "Sendspin render feedback PSRAM task create failed; retrying internal stack");
#endif
    BaseType_t fallback_created = xTaskCreatePinnedToCore(
        render_feedback_task,
        "sendspin_render",
        SENDSPIN_RENDER_TASK_STACK_BYTES,
        listener,
        SENDSPIN_RENDER_TASK_PRIORITY,
        &s_render_feedback_task_handle,
        1
    );
    if (fallback_created != pdPASS) {
        ESP_LOGE(TAG, "Sendspin render feedback task create failed");
        return false;
    }
    ESP_LOGI(
        TAG,
        "Sendspin render feedback task started priority=%u core=1 stack=internal",
        SENDSPIN_RENDER_TASK_PRIORITY
    );
    return true;
}

bool start_mdns_advertisement() {
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_hostname_set(s_client_id));
    ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_instance_name_set(s_friendly_name));
    mdns_txt_item_t txt[] = {
        {"path", "/sendspin"},
        {"name", s_friendly_name},
    };
    err = mdns_service_add(
        s_friendly_name,
        "_sendspin",
        "_tcp",
        SENDSPIN_PORT,
        txt,
        sizeof(txt) / sizeof(txt[0])
    );
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Sendspin mDNS advertisement failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "advertising _sendspin._tcp port=%u name=%s", SENDSPIN_PORT, s_friendly_name);
    return true;
}

void sendspin_task(void *arg) {
    (void)arg;

    SendspinClientConfig client_config;
    client_config.name = s_friendly_name;
    client_config.product_name = TATER_BOARD_DISPLAY_NAME;
    client_config.manufacturer = "Tater";
    client_config.software_version = TATER_FIRMWARE_VERSION;
    client_config.server_port = SENDSPIN_PORT;
    // The HTTPD task is the hot receive path for PCM WebSocket frames. Keeping
    // its small stack internal prevents multi-second TCP backpressure stalls;
    // the larger protocol, player, and application task stacks remain in PSRAM.
    client_config.httpd_psram_stack = false;
    // Keep the websocket receiver ahead of the sync/decode worker so incoming
    // PCM cannot sit in the TCP queue while lower-priority work is running.
    client_config.httpd_priority = SENDSPIN_HTTPD_TASK_PRIORITY;
    // Noise decryption and inbound routing also run for every PCM frame. Keep
    // this hot stack internal; larger non-hot application stacks are in PSRAM.
    client_config.protocol_task_psram_stack = false;
    client_config.protocol_task_priority = SENDSPIN_PROTOCOL_TASK_PRIORITY;
    client_config.inbound_ring_location = MemoryLocation::PREFER_EXTERNAL;
    client_config.noise_buffer_location = MemoryLocation::PREFER_EXTERNAL;

    SendspinClient client(std::move(client_config));
    TaterNetworkProvider network_provider;
    TaterPersistenceProvider persistence_provider;
    TaterClientListener client_listener;
    client.set_listener(&client_listener);
    client.set_network_provider(&network_provider);
    client.set_persistence_provider(&persistence_provider);
    // Tater and third-party servers can start playback without an interactive
    // pairing step, matching the LAN behavior of the previous Sendspin client.
    client.set_unpaired_access_enabled(true);

    PlayerRoleConfig player_config;
    // Prefer lossless compression for sustained music playback. Raw 48 kHz
    // stereo PCM can saturate the ESP32 receive path during normal radio/CPU
    // contention; FLAC cuts that traffic substantially. Keep PCM as a fallback
    // for Tater replies and servers without a FLAC encoder.
    player_config.audio_formats = {
        {SendspinCodecFormat::FLAC, TATER_SPK_CHANNELS, TATER_SPK_SAMPLE_RATE, 16},
        {SendspinCodecFormat::PCM, TATER_SPK_CHANNELS, TATER_SPK_SAMPLE_RATE, 16},
    };
    player_config.audio_buffer_capacity = SENDSPIN_AUDIO_BUFFER_BYTES;
    player_config.min_buffer_ms = SENDSPIN_MIN_BUFFER_MS;
    const int32_t fixed_output_delay_us = static_cast<int32_t>(
        (static_cast<int64_t>(TATER_MEDIA_RENDER_LATENCY_FRAMES) * 1000000LL)
        / TATER_SPK_SAMPLE_RATE
    );
    player_config.fixed_delay_us = fixed_output_delay_us;
    player_config.extra_startup_silence_ms = SENDSPIN_STARTUP_HEADROOM_MS;
    player_config.priority = SENDSPIN_SYNC_TASK_PRIORITY;
    player_config.psram_stack = true;
    player_config.decode_buffer_location = MemoryLocation::PREFER_EXTERNAL;

    PlayerRole &player = client.add_player(std::move(player_config));
    // Every native board has its own fixed hardware-pipeline compensation.
    // Keep the optional server-adjustable delay disabled until the native
    // clients can advertise it without destabilizing Music Assistant streams.
    player.set_output_delay_adjustable(TATER_SENDSPIN_OUTPUT_DELAY_ADJUSTABLE);
    TaterPlayerListener player_listener(&player);
    player.set_listener(&player_listener);

#if !TATER_BOARD_S3_BOX
    TaterColorListener color_listener;
    client.add_color().set_listener(&color_listener);

    VisualizerRoleConfig visualizer_config;
    visualizer_config.support.buffer_capacity = 16 * 1024;
    visualizer_config.stream.types = {
        VisualizerDataType::LOUDNESS,
        VisualizerDataType::BEAT,
        VisualizerDataType::PEAK,
        VisualizerDataType::SPECTRUM,
    };
    // Ten synchronized updates per second matches the Echo ring client and is
    // plenty for the LED envelope while keeping spectrum traffic and parsing
    // out of the ESP audio hot path.
    visualizer_config.stream.rate_max = 10;
    visualizer_config.stream.spectrum = VisualizerSpectrumConfig{
        .n_disp_bins = TATER_LED_COUNT,
        .scale = VisualizerSpectrumScale::LOG,
        .f_min = 40,
        .f_max = 16000,
    };
    visualizer_config.psram_stack = true;
    TaterVisualizerListener visualizer_listener;
    client.add_visualizer(std::move(visualizer_config)).set_listener(&visualizer_listener);
#endif

    if (!client.start()) {
        ESP_LOGE(TAG, "Sendspin client failed to start");
        s_initialized.store(false, std::memory_order_release);
#if (configSUPPORT_STATIC_ALLOCATION == 1)
        if (s_sendspin_task_stack_psram.load(std::memory_order_acquire)) {
            vTaskDeleteWithCaps(nullptr);
            return;
        }
#endif
        vTaskDelete(nullptr);
        return;
    }
    s_output_delay_ms.store(player.get_output_delay_ms(), std::memory_order_release);
    ESP_LOGI(
        TAG,
        "Sendspin v0.9 client=%s timing fixed=%ld us adjustable=%u ms",
        client.client_id().c_str(),
        static_cast<long>(fixed_output_delay_us),
        static_cast<unsigned>(player.get_output_delay_ms())
    );
    player.update_volume(s_desired_volume.load(std::memory_order_acquire));
    bool render_feedback_started = start_render_feedback_task(&player_listener);

    bool mdns_started = false;
    bool native_state_reported = false;
    while (true) {
        client.loop();
        if (!render_feedback_started) {
            player_listener.poll_render_clock();
        }

        bool native_claimed = native_audio_claimed();
        if (native_claimed != native_state_reported) {
            client.set_available(!native_claimed);
            native_state_reported = native_claimed;
        }
        if (native_claimed && s_stream_active.load(std::memory_order_acquire)) {
            s_disconnect_requested.store(true, std::memory_order_release);
        }
        if (s_disconnect_requested.exchange(false, std::memory_order_acq_rel)
            && client.is_connected()) {
            client.disconnect(SendspinGoodbyeReason::USER_REQUEST);
        }
        if (s_volume_dirty.load(std::memory_order_acquire)) {
            TickType_t now = xTaskGetTickCount();
            TickType_t changed = s_volume_changed_tick.load(std::memory_order_acquire);
            if ((now - changed) >= SENDSPIN_VOLUME_PUBLISH_SETTLE_TICKS
                && s_volume_dirty.exchange(false, std::memory_order_acq_rel)) {
                // Audio gain follows s_output_volume immediately. Only coalesce the network state
                // publication so a fast wheel spin sends one final Sendspin volume snapshot.
                player.update_volume(s_desired_volume.load(std::memory_order_acquire));
            }
        }
        if (!mdns_started && tater_wifi_is_connected()) {
            mdns_started = start_mdns_advertisement();
        }
        vTaskDelay(SENDSPIN_LOOP_TICKS);
    }
}

}  // namespace

extern "C" bool IRAM_ATTR tater_sendspin_notify_render_from_isr(void) {
    TaskHandle_t task = s_render_feedback_task_handle;
    if (!task) {
        return false;
    }
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

extern "C" esp_err_t tater_sendspin_init(const tater_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized.exchange(true, std::memory_order_acq_rel)) {
        return ESP_OK;
    }

    const char *device_id = tater_protocol_device_id();
    const char *device_name = config->device_name[0] ? config->device_name : device_id;
    strlcpy(s_client_id, device_id && device_id[0] ? device_id : TATER_DEVICE_ID_PREFIX,
            sizeof(s_client_id));
    strlcpy(s_friendly_name, device_name && device_name[0] ? device_name : TATER_DEFAULT_DEVICE_NAME,
            sizeof(s_friendly_name));
    const tater_live_settings_t *settings = tater_live_settings_get();
    uint8_t volume = settings ? settings->volume_percent : 80;
    s_output_volume.store(volume, std::memory_order_release);
    s_desired_volume.store(volume, std::memory_order_release);
    s_voice_duck_active.store(false, std::memory_order_release);
    s_overlay_active.store(false, std::memory_order_release);
    reset_overlay_ring();
    if (!s_overlay_samples) {
        size_t overlay_bytes = SENDSPIN_OVERLAY_CAPACITY_FRAMES
            * TATER_SPK_CHANNELS * sizeof(int16_t);
        s_overlay_samples = static_cast<int16_t *>(heap_caps_calloc(
            1,
            overlay_bytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        ));
        if (!s_overlay_samples) {
            s_overlay_samples = static_cast<int16_t *>(heap_caps_calloc(
                1,
                overlay_bytes,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
            ));
        }
        if (s_overlay_samples) {
            ESP_LOGI(
                TAG,
                "Sendspin native overlay buffer ready frames=%u bytes=%u",
                static_cast<unsigned>(SENDSPIN_OVERLAY_CAPACITY_FRAMES),
                static_cast<unsigned>(overlay_bytes)
            );
        } else {
            ESP_LOGW(TAG, "Sendspin native overlay buffer unavailable");
        }
    }
    tater_sendspin_output_channel_t output_channel = load_output_channel();
    s_output_channel.store(static_cast<uint8_t>(output_channel), std::memory_order_release);
    ESP_LOGI(TAG, "Sendspin output channel mode=%s", output_channel_name(output_channel));

    BaseType_t created = pdFAIL;
#if (configSUPPORT_STATIC_ALLOCATION == 1)
    s_sendspin_task_stack_psram.store(true, std::memory_order_release);
    created = xTaskCreatePinnedToCoreWithCaps(
        sendspin_task,
        "tater_sendspin",
        8192,
        nullptr,
        5,
        nullptr,
        0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
#endif
    if (created != pdPASS) {
        s_sendspin_task_stack_psram.store(false, std::memory_order_release);
        created = xTaskCreatePinnedToCore(
            sendspin_task,
            "tater_sendspin",
            8192,
            nullptr,
            5,
            nullptr,
            0
        );
    }
    if (created != pdPASS) {
        s_initialized.store(false, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(
        TAG,
        "Sendspin main task started core=0 stack=%s",
        s_sendspin_task_stack_psram.load(std::memory_order_acquire) ? "psram" : "internal"
    );
    return ESP_OK;
}

extern "C" bool tater_sendspin_claim_native_audio(uint32_t timeout_ms) {
    if (!s_initialized.load(std::memory_order_acquire)) {
        return true;
    }
    s_native_audio_claim_count.fetch_add(1, std::memory_order_acq_rel);
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (s_speaker_owned.load(std::memory_order_acquire)) {
        if (timeout_ticks == 0 || (xTaskGetTickCount() - started) >= timeout_ticks) {
            ESP_LOGW(TAG, "timed out waiting for Sendspin to release native audio");
            s_native_audio_claim_count.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

extern "C" void tater_sendspin_release_native_audio(void) {
    uint32_t claims = s_native_audio_claim_count.load(std::memory_order_acquire);
    while (claims != 0) {
        if (s_native_audio_claim_count.compare_exchange_weak(
                claims,
                claims - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return;
        }
    }
}

extern "C" bool tater_sendspin_begin_native_overlay(void) {
    if (!s_initialized.load(std::memory_order_acquire)
        || !s_overlay_samples
        || !s_stream_active.load(std::memory_order_acquire)
        || !s_speaker_owned.load(std::memory_order_acquire)
        || native_audio_claimed()
        || s_overlay_active.load(std::memory_order_acquire)) {
        return false;
    }

    reset_overlay_ring();
    s_overlay_active.store(true, std::memory_order_release);
    publish_duck_change();
    ESP_LOGI(TAG, "Sendspin native overlay started");
    return true;
}

extern "C" esp_err_t tater_sendspin_write_native_overlay(
    const int16_t *stereo_frames,
    size_t frame_count,
    uint32_t timeout_ms
) {
    if (!stereo_frames || frame_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_overlay_active.load(std::memory_order_acquire) || !s_overlay_samples) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t started = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    size_t written_frames = 0;
    while (written_frames < frame_count) {
        if (!s_overlay_active.load(std::memory_order_acquire)
            || !s_stream_active.load(std::memory_order_acquire)
            || !s_speaker_owned.load(std::memory_order_acquire)) {
            return ESP_ERR_INVALID_STATE;
        }

        uint32_t read_frame = s_overlay_read_frame.load(std::memory_order_acquire);
        uint32_t write_frame = s_overlay_write_frame.load(std::memory_order_relaxed);
        size_t used = static_cast<size_t>(write_frame - read_frame);
        size_t space = used < SENDSPIN_OVERLAY_CAPACITY_FRAMES
            ? SENDSPIN_OVERLAY_CAPACITY_FRAMES - used
            : 0;
        if (space == 0) {
            if (timeout_ticks == 0 || (xTaskGetTickCount() - started) >= timeout_ticks) {
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        size_t ring_frame = static_cast<size_t>(write_frame)
            % SENDSPIN_OVERLAY_CAPACITY_FRAMES;
        size_t contiguous = std::min(
            frame_count - written_frames,
            std::min(space, SENDSPIN_OVERLAY_CAPACITY_FRAMES - ring_frame)
        );
        std::memcpy(
            s_overlay_samples + (ring_frame * TATER_SPK_CHANNELS),
            stereo_frames + (written_frames * TATER_SPK_CHANNELS),
            contiguous * TATER_SPK_CHANNELS * sizeof(int16_t)
        );
        write_frame += static_cast<uint32_t>(contiguous);
        s_overlay_write_frame.store(write_frame, std::memory_order_release);
        written_frames += contiguous;
    }
    return ESP_OK;
}

extern "C" esp_err_t tater_sendspin_end_native_overlay(uint32_t timeout_ms) {
    if (!s_overlay_active.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t started = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (s_overlay_active.load(std::memory_order_acquire)
        && s_stream_active.load(std::memory_order_acquire)
        && s_overlay_read_frame.load(std::memory_order_acquire)
            != s_overlay_write_frame.load(std::memory_order_acquire)) {
        if (timeout_ticks == 0 || (xTaskGetTickCount() - started) >= timeout_ticks) {
            ESP_LOGW(TAG, "Sendspin native overlay drain timed out");
            stop_overlay_immediately();
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (!s_overlay_active.load(std::memory_order_acquire)
        || !s_stream_active.load(std::memory_order_acquire)) {
        stop_overlay_immediately();
        return ESP_ERR_INVALID_STATE;
    }

    s_overlay_active.store(false, std::memory_order_release);
    reset_overlay_ring();
    publish_duck_change();
    ESP_LOGI(TAG, "Sendspin native overlay finished");
    return ESP_OK;
}

extern "C" void tater_sendspin_abort_native_overlay(void) {
    stop_overlay_immediately();
}

extern "C" void tater_sendspin_set_voice_duck(bool active) {
    bool previous = s_voice_duck_active.exchange(active, std::memory_order_acq_rel);
    if (previous != active) {
        publish_duck_change();
    }
}

extern "C" void tater_sendspin_set_volume(uint8_t volume_percent) {
    volume_percent = std::min<uint8_t>(volume_percent, 100);
    s_output_volume.store(volume_percent, std::memory_order_release);
    s_desired_volume.store(volume_percent, std::memory_order_release);
    if (!s_applying_server_volume.load(std::memory_order_acquire)) {
        s_volume_changed_tick.store(xTaskGetTickCount(), std::memory_order_release);
        s_volume_dirty.store(true, std::memory_order_release);
    }
}

extern "C" bool tater_sendspin_set_output_channel_mode(const char *mode) {
    tater_sendspin_output_channel_t channel = TATER_SENDSPIN_OUTPUT_STEREO;
    if (!parse_output_channel(mode, &channel)) {
        return false;
    }
    uint8_t next = static_cast<uint8_t>(channel);
    uint8_t previous = s_output_channel.exchange(next, std::memory_order_acq_rel);
    if (previous != next) {
        if (!save_output_channel(channel)) {
            ESP_LOGW(TAG, "could not persist Sendspin output channel mode=%s", output_channel_name(channel));
        }
        ESP_LOGI(TAG, "Sendspin output channel mode=%s", output_channel_name(channel));
    }
    return true;
}

extern "C" const char *tater_sendspin_output_channel_mode(void) {
    return output_channel_name(static_cast<tater_sendspin_output_channel_t>(
        s_output_channel.load(std::memory_order_acquire)
    ));
}

extern "C" bool tater_sendspin_is_playing(void) {
    return s_stream_active.load(std::memory_order_acquire);
}

extern "C" void tater_sendspin_stats_snapshot(tater_sendspin_stats_t *stats) {
    if (!stats) {
        return;
    }
    std::memset(stats, 0, sizeof(*stats));
    stats->active = s_stream_active.load(std::memory_order_acquire);
    stats->streams_started = s_streams_started.load(std::memory_order_relaxed);
    stats->streams_completed = s_streams_completed.load(std::memory_order_relaxed);
    stats->audio_write_failures = s_audio_write_failures.load(std::memory_order_relaxed);
    stats->render_clock_failures = s_render_clock_failures.load(std::memory_order_relaxed);
    stats->output_starvations = s_output_starvations.load(std::memory_order_relaxed);
    stats->last_starvation_us = s_last_starvation_us.load(std::memory_order_relaxed);
    stats->max_starvation_us = s_max_starvation_us.load(std::memory_order_relaxed);
    stats->max_queued_frames = s_max_queued_frames.load(std::memory_order_relaxed);
    stats->time_sync_updates = s_time_sync_updates.load(std::memory_order_relaxed);
    stats->clock_error_us = s_clock_error_us.load(std::memory_order_relaxed);
    stats->max_clock_error_us = s_max_clock_error_us.load(std::memory_order_relaxed);
    stats->startup_headroom_ms = SENDSPIN_STARTUP_HEADROOM_MS;
    stats->output_delay_ms = s_output_delay_ms.load(std::memory_order_relaxed);
    stats->fixed_output_delay_us = static_cast<int32_t>(
        (static_cast<int64_t>(TATER_MEDIA_RENDER_LATENCY_FRAMES) * 1000000LL)
        / TATER_SPK_SAMPLE_RATE
    );
    stats->output_delay_adjustable = TATER_SENDSPIN_OUTPUT_DELAY_ADJUSTABLE;
    stats->sync_task_priority = SENDSPIN_SYNC_TASK_PRIORITY;
}
