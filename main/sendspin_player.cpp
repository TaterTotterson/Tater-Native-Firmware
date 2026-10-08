#include "sendspin_player.h"

#include <algorithm>
#include <atomic>
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
#include "esp_pthread.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "sendspin/client.h"
#include "sendspin/player_role.h"

extern "C" {
#include "audio_i2s.h"
#include "native_settings.h"
#include "playback_mix.h"
#include "tater_protocol.h"
#include "wifi_station.h"
}

namespace {

using sendspin::MemoryLocation;
using sendspin::PlayerRole;
using sendspin::PlayerRoleConfig;
using sendspin::PlayerRoleListener;
using sendspin::SendspinClient;
using sendspin::SendspinClientConfig;
using sendspin::SendspinClientListener;
using sendspin::SendspinClientState;
using sendspin::SendspinCodecFormat;
using sendspin::SendspinGoodbyeReason;
using sendspin::SendspinNetworkProvider;
using sendspin::SendspinPersistenceProvider;

constexpr uint16_t SENDSPIN_PORT = 8928;
constexpr size_t SENDSPIN_AUDIO_BUFFER_BYTES = 512 * 1024;
constexpr size_t SENDSPIN_SYNC_STACK_BYTES = 8192;
constexpr uint16_t SENDSPIN_STARTUP_HEADROOM_MS = 120;
constexpr unsigned SENDSPIN_SYNC_TASK_PRIORITY = 7;
constexpr int64_t SENDSPIN_STARVATION_MIN_US = 10000;
constexpr TickType_t SENDSPIN_LOOP_TICKS = pdMS_TO_TICKS(5);
constexpr const char *SENDSPIN_NVS_NAMESPACE = "sendspin";
constexpr const char *SENDSPIN_NVS_LAST_SERVER = "last_server";
constexpr const char *SENDSPIN_NVS_OUTPUT_DELAY = "out_delay";
constexpr const char *SENDSPIN_NVS_OUTPUT_CHANNEL = "channel_mode";

const char *TAG = "tater_sendspin";

std::atomic<bool> s_initialized{false};
std::atomic<uint32_t> s_native_audio_claim_count{0};
std::atomic<bool> s_stream_active{false};
std::atomic<bool> s_speaker_owned{false};
std::atomic<bool> s_disconnect_requested{false};
std::atomic<bool> s_volume_dirty{false};
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
std::atomic<int64_t> s_output_empty_since_us{0};
std::atomic<bool> s_received_stream_audio{false};

char s_friendly_name[TATER_CFG_DEVICE_NAME_LEN] = {};
char s_client_id[48] = {};

bool configure_sendspin_sync_stack(unsigned priority) {
    esp_pthread_cfg_t config = esp_pthread_get_default_config();
    config.stack_size = SENDSPIN_SYNC_STACK_BYTES;
    config.prio = priority;
    config.thread_name = "Sendspin";
    config.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    esp_err_t err = esp_pthread_set_cfg(&config);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "could not configure Sendspin PSRAM stack: %s; using internal stack",
            esp_err_to_name(err)
        );
        return false;
    }

    // sendspin-cpp 0.8.0 asks ESP-IDF for MALLOC_CAP_SPIRAM without the
    // required MALLOC_CAP_8BIT flag. ESP-IDF rejects that later request and
    // retains this valid per-task configuration for the sync/decode pthread.
    ESP_LOGI(
        TAG,
        "Sendspin sync stack configured bytes=%u memory=psram",
        static_cast<unsigned>(SENDSPIN_SYNC_STACK_BYTES)
    );
    return true;
}

bool native_audio_claimed() {
    return s_native_audio_claim_count.load(std::memory_order_acquire) != 0;
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
    bool save_last_server_hash(uint32_t hash) override {
        return save_u32(SENDSPIN_NVS_LAST_SERVER, hash);
    }

    std::optional<uint32_t> load_last_server_hash() override {
        nvs_handle_t handle = 0;
        if (nvs_open(SENDSPIN_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
            return std::nullopt;
        }
        uint32_t value = 0;
        esp_err_t err = nvs_get_u32(handle, SENDSPIN_NVS_LAST_SERVER, &value);
        nvs_close(handle);
        if (err != ESP_OK) {
            return std::nullopt;
        }
        return value;
    }

    bool save_static_delay(uint16_t delay_ms) override {
        nvs_handle_t handle = 0;
        if (nvs_open(SENDSPIN_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
            return false;
        }
        esp_err_t err = nvs_set_u16(handle, SENDSPIN_NVS_OUTPUT_DELAY, delay_ms);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        return err == ESP_OK;
    }

    std::optional<uint16_t> load_static_delay() override {
        nvs_handle_t handle = 0;
        if (nvs_open(SENDSPIN_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
            return std::nullopt;
        }
        uint16_t value = 0;
        esp_err_t err = nvs_get_u16(handle, SENDSPIN_NVS_OUTPUT_DELAY, &value);
        nvs_close(handle);
        if (err != ESP_OK) {
            return std::nullopt;
        }
        return value;
    }

private:
    static bool save_u32(const char *key, uint32_t value) {
        nvs_handle_t handle = 0;
        if (nvs_open(SENDSPIN_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
            return false;
        }
        esp_err_t err = nvs_set_u32(handle, key, value);
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

class TaterPlayerListener final : public PlayerRoleListener {
public:
    explicit TaterPlayerListener(PlayerRole *player) : player_(player) {}

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

        esp_err_t err = tater_audio_write_speaker_frames(samples, frames);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Sendspin I2S write failed: %s", esp_err_to_name(err));
            s_audio_write_failures.fetch_add(1, std::memory_order_relaxed);
            s_disconnect_requested.store(true, std::memory_order_release);
            return 0;
        }
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
            ESP_LOGE(TAG, "unsupported negotiated Sendspin PCM format");
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
        player_->notify_audio_played(played_audio_frames, esp_timer_get_time());
    }

private:
    PlayerRole *player_;
    uint32_t last_completed_frames_{0};
    uint32_t pending_prefix_frames_{0};
    std::atomic<uint64_t> notified_frames_{0};
};

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
    client_config.client_id = s_client_id;
    client_config.name = s_friendly_name;
    client_config.product_name = TATER_BOARD_DISPLAY_NAME;
    client_config.manufacturer = "Tater";
    client_config.software_version = TATER_FIRMWARE_VERSION;
    client_config.server_port = SENDSPIN_PORT;
    client_config.httpd_psram_stack = true;
    client_config.websocket_payload_location = MemoryLocation::PREFER_EXTERNAL;

    SendspinClient client(std::move(client_config));
    PlayerRoleConfig player_config;
    player_config.audio_formats = {
        {SendspinCodecFormat::FLAC, TATER_SPK_CHANNELS, TATER_SPK_SAMPLE_RATE, 16},
        {SendspinCodecFormat::PCM, TATER_SPK_CHANNELS, TATER_SPK_SAMPLE_RATE, 16},
    };
    player_config.audio_buffer_capacity = SENDSPIN_AUDIO_BUFFER_BYTES;
    player_config.fixed_delay_us = static_cast<int32_t>(
        (static_cast<int64_t>(TATER_MEDIA_RENDER_LATENCY_FRAMES) * 1000000LL)
        / TATER_SPK_SAMPLE_RATE
    );
    player_config.extra_startup_silence_ms = SENDSPIN_STARTUP_HEADROOM_MS;
    player_config.priority = SENDSPIN_SYNC_TASK_PRIORITY;
    player_config.psram_stack = configure_sendspin_sync_stack(player_config.priority);
    player_config.decode_buffer_location = MemoryLocation::PREFER_EXTERNAL;

    PlayerRole &player = client.add_player(std::move(player_config));
    TaterPlayerListener player_listener(&player);
    TaterNetworkProvider network_provider;
    TaterPersistenceProvider persistence_provider;
    TaterClientListener client_listener;
    player.set_listener(&player_listener);
    client.set_listener(&client_listener);
    client.set_network_provider(&network_provider);
    client.set_persistence_provider(&persistence_provider);

    if (!client.start()) {
        ESP_LOGE(TAG, "Sendspin client failed to start");
        s_initialized.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }
    player.update_volume(s_desired_volume.load(std::memory_order_acquire));

    bool mdns_started = false;
    bool native_state_reported = false;
    while (true) {
        client.loop();
        player_listener.poll_render_clock();

        bool native_claimed = native_audio_claimed();
        if (native_claimed != native_state_reported) {
            client.update_state(
                native_claimed
                    ? SendspinClientState::EXTERNAL_SOURCE
                    : SendspinClientState::SYNCHRONIZED
            );
            native_state_reported = native_claimed;
        }
        if (native_claimed && s_stream_active.load(std::memory_order_acquire)) {
            s_disconnect_requested.store(true, std::memory_order_release);
        }
        if (s_disconnect_requested.exchange(false, std::memory_order_acq_rel)
            && client.is_connected()) {
            client.disconnect(SendspinGoodbyeReason::USER_REQUEST);
        }
        if (s_volume_dirty.exchange(false, std::memory_order_acq_rel)) {
            player.update_volume(s_desired_volume.load(std::memory_order_acquire));
        }
        if (!mdns_started && tater_wifi_is_connected()) {
            mdns_started = start_mdns_advertisement();
        }
        vTaskDelay(SENDSPIN_LOOP_TICKS);
    }
}

}  // namespace

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
    tater_sendspin_output_channel_t output_channel = load_output_channel();
    s_output_channel.store(static_cast<uint8_t>(output_channel), std::memory_order_release);
    ESP_LOGI(TAG, "Sendspin output channel mode=%s", output_channel_name(output_channel));

    BaseType_t created = xTaskCreatePinnedToCore(
        sendspin_task,
        "tater_sendspin",
        8192,
        nullptr,
        5,
        nullptr,
        0
    );
    if (created != pdPASS) {
        s_initialized.store(false, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
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

extern "C" void tater_sendspin_set_volume(uint8_t volume_percent) {
    volume_percent = std::min<uint8_t>(volume_percent, 100);
    s_output_volume.store(volume_percent, std::memory_order_release);
    s_desired_volume.store(volume_percent, std::memory_order_release);
    if (!s_applying_server_volume.load(std::memory_order_acquire)) {
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
    stats->sync_task_priority = SENDSPIN_SYNC_TASK_PRIORITY;
}
