#include "playback.h"

#include <inttypes.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_i2s.h"
#include "board.h"
#include "esp_audio_simple_dec.h"
#include "esp_flac_dec.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "native_settings.h"
#include "playback_mix.h"
#include "playback_recovery.h"
#include "playback_sync.h"
#include "tater_protocol.h"

static const char *TAG = "tater_playback";
static const size_t PLAYBACK_HTTP_READ_SIZE = 8192;
static const size_t PLAYBACK_HEADER_LIMIT = 16 * 1024;
static const size_t PLAYBACK_CODEC_OUT_INITIAL = 4096;
static const size_t PLAYBACK_WAV_JITTER_CAPACITY = 512 * 1024;
static const size_t PLAYBACK_WAV_PREBUFFER_SMALL = 32 * 1024;
static const size_t PLAYBACK_WAV_PREBUFFER_MEDIUM = 64 * 1024;
static const size_t PLAYBACK_WAV_PREBUFFER_LARGE = 128 * 1024;
static const size_t PLAYBACK_MP3_JITTER_CAPACITY = 128 * 1024;
static const size_t PLAYBACK_MP3_PREBUFFER = 64 * 1024;
static const size_t PLAYBACK_FLAC_JITTER_CAPACITY = 512 * 1024;
static const size_t PLAYBACK_FLAC_PREBUFFER = 256 * 1024;
static const uint32_t PLAYBACK_HTTP_READER_TASK_STACK = 4096;
static const uint32_t PLAYBACK_URL_TASK_STACK = 16384;
static const uint32_t PLAYBACK_TONE_TASK_STACK = 8192;
static const uint32_t PLAYBACK_SCENE_BACKGROUND_TASK_STACK = 16384;
static const uint32_t PLAYBACK_STOP_WAIT_MS = 3000;
static const uint32_t PLAYBACK_STOP_POLL_MS = 20;
static const uint32_t PLAYBACK_SCENE_PREBUFFER_MS = 250;
static const uint32_t PLAYBACK_SCENE_PREBUFFER_TIMEOUT_MS = 5000;
static const uint32_t PLAYBACK_MEDIA_PREPARE_TIMEOUT_MS = 30000;
static const uint32_t PLAYBACK_MEDIA_PLAYHEAD_INTERVAL_MS = 1000;
static const uint32_t PLAYBACK_MEDIA_REBUFFER_MS = 250;
static const uint32_t PLAYBACK_RECOVERY_POLL_MS = 500;
static const uint32_t PLAYBACK_RECOVERY_STALL_MS = 5000;
static const uint32_t PLAYBACK_RECOVERY_TASK_STACK = 4096;
static const uint32_t PLAYBACK_MEDIA_REJOIN_TOLERANCE_FRAMES = 24;
static const uint32_t PLAYBACK_MEDIA_REJOIN_FADE_MS = 20;
static const int32_t PLAYBACK_MEDIA_MAX_PENDING_CORRECTION_FRAMES = 480;
static const size_t PLAYBACK_SCENE_BACKGROUND_RING_FRAMES = 2 * TATER_SPK_SAMPLE_RATE;
static const size_t PLAYBACK_MEDIA_RING_FRAMES = 2 * TATER_SPK_SAMPLE_RATE;
static const size_t PLAYBACK_OVERLAY_RING_FRAMES = TATER_SPK_SAMPLE_RATE;
#define PLAYBACK_MIX_CHUNK_FRAMES 256
#ifndef TATER_MEDIA_PLAYBACK_TASK_PRIORITY
#define TATER_MEDIA_PLAYBACK_TASK_PRIORITY 5
#endif

typedef struct playback_pcm_sink playback_pcm_sink_t;

struct playback_pcm_sink {
    void *ctx;
    esp_err_t (*begin)(void *ctx);
    esp_err_t (*write)(void *ctx, const int16_t *stereo_frames, size_t frame_count);
    esp_err_t (*end)(void *ctx);
    uint8_t volume_percent;
    volatile uint8_t *live_volume_percent;
    bool absolute_volume;
};

typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    const uint8_t *data;
    size_t data_len;
    size_t frame_count;
} wav_info_t;

typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_len;
    size_t bytes_per_frame;
} wav_stream_info_t;

typedef enum {
    WAV_HEADER_NEED_MORE = 0,
    WAV_HEADER_OK,
    WAV_HEADER_INVALID,
} wav_header_result_t;

typedef struct {
    playback_pcm_sink_t *sink;
    uint8_t partial_frame[32];
    size_t partial_len;
    size_t data_bytes_seen;
    uint64_t resample_accum;
    int16_t out[256 * TATER_SPK_CHANNELS];
    size_t out_frames;
    uint32_t input_frames;
    uint32_t output_frames;
} pcm_stream_state_t;

typedef pcm_stream_state_t wav_stream_state_t;

typedef enum {
    STREAM_AUDIO_UNKNOWN = 0,
    STREAM_AUDIO_WAV,
    STREAM_AUDIO_MP3,
    STREAM_AUDIO_FLAC,
} stream_audio_type_t;

typedef struct {
    esp_audio_simple_dec_handle_t decoder;
    esp_audio_simple_dec_type_t type;
    uint8_t *out_buf;
    size_t out_cap;
    esp_audio_simple_dec_info_t info;
    bool have_info;
    bool sink_started;
    playback_pcm_sink_t *sink;
    pcm_stream_state_t pcm;
    uint32_t decoded_bytes;
} codec_stream_state_t;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t fill;
    size_t total_written;
    size_t total_read;
    size_t high_water;
    bool eof;
    bool failed;
    esp_err_t error;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t can_read;
    SemaphoreHandle_t can_write;
} codec_jitter_buffer_t;

typedef struct {
    esp_http_client_handle_t client;
    codec_jitter_buffer_t *buffer;
    uint8_t *read_buf;
    size_t read_size;
    int64_t content_length;
    size_t bytes_seen;
    TaskHandle_t notify_task;
    bool task_with_caps;
    volatile bool done;
} codec_http_reader_args_t;

typedef struct {
    uint32_t frequency_hz;
    uint32_t duration_ms;
    uint8_t volume_percent;
    bool notify_finished;
    bool task_with_caps;
} tone_args_t;

typedef struct {
    char *url;
    bool notify_finished;
    bool task_with_caps;
} playback_args_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    char label[64];
    bool free_data;
    bool task_with_caps;
} playback_memory_args_t;

typedef struct {
    int16_t *samples;
    size_t capacity_frames;
    size_t read_frame;
    size_t write_frame;
    size_t fill_frames;
    size_t high_water_frames;
    size_t underrun_frames;
    size_t total_written_frames;
    size_t total_read_frames;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t can_read;
    SemaphoreHandle_t can_write;
    volatile bool stop;
    bool closed;
    bool failed;
} scene_pcm_ring_t;

typedef struct {
    scene_pcm_ring_t *ring;
    char *url;
    bool loop;
    uint8_t volume_percent;
    volatile uint8_t *live_volume_percent;
    bool absolute_volume;
    uint64_t skip_frames_remaining;
    uint64_t skipped_frames;
    volatile bool done;
    bool task_with_caps;
    esp_err_t result;
    TaskHandle_t notify_task;
} scene_background_args_t;

typedef struct {
    scene_pcm_ring_t *background;
    tater_playback_mix_state_t mix;
    uint8_t ducking_target_percent;
    uint16_t ducking_attack_ms;
    uint16_t ducking_release_ms;
    uint16_t fade_out_ms;
    bool speaker_started;
} scene_mixer_sink_t;

typedef struct {
    char scene_id[TATER_PLAYBACK_SCENE_ID_MAX];
    char *foreground_url;
    char *background_url;
    uint8_t foreground_volume_percent;
    uint8_t background_volume_percent;
    uint8_t ducking_target_percent;
    uint16_t ducking_attack_ms;
    uint16_t ducking_release_ms;
    uint16_t background_fade_out_ms;
    bool background_loop;
    bool task_with_caps;
} scene_args_t;

typedef struct {
    SemaphoreHandle_t lock;
    bool active;
    bool accepting_overlays;
    char session_id[TATER_PLAYBACK_MEDIA_SESSION_ID_MAX];
    char group_id[TATER_PLAYBACK_MEDIA_GROUP_ID_MAX];
    char prepare_reply_to[TATER_PLAYBACK_REQUEST_ID_MAX];
    char *media_url;
    uint8_t media_volume_percent;
    uint32_t media_start_position_ms;
    tater_playback_channel_t media_channel;
    bool media_loop;
    bool complete_visual_state;
    bool tool_visual_state;
    bool prepare_requested;
    bool prepared;
    bool committed;
    int64_t scheduled_start_us;
    tater_playback_sync_slew_t correction_slew;
    int32_t pending_jump_frames;
    uint64_t source_frames;
    uint64_t output_frames;
    scene_pcm_ring_t media_ring;
    bool media_ring_initialized;
    scene_background_args_t *media_decoder;
    TaskHandle_t media_decoder_task;
    bool task_with_caps;

    char overlay_id[TATER_PLAYBACK_OVERLAY_ID_MAX];
    char *overlay_url;
    uint8_t overlay_volume_percent;
    uint8_t ducking_target_percent;
    uint16_t ducking_attack_ms;
    uint16_t ducking_release_ms;
    scene_pcm_ring_t overlay_ring;
    bool overlay_ring_initialized;
    scene_background_args_t *overlay_decoder;
    TaskHandle_t overlay_decoder_task;
    bool overlay_pending;
    bool overlay_active;
    bool overlay_releasing;
    bool overlay_started_reported;
    int64_t overlay_start_at_us;
} media_session_state_t;

static volatile bool s_abort;
static volatile bool s_playing;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lifecycle_lock;
static media_session_state_t s_media_session;
static TaskHandle_t s_recovery_task;
static portMUX_TYPE s_recovery_lock = portMUX_INITIALIZER_UNLOCKED;
static tater_playback_recovery_state_t s_recovery_state;

static uint32_t playback_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void playback_recovery_begin_session(void)
{
    portENTER_CRITICAL(&s_recovery_lock);
    tater_playback_recovery_reset(&s_recovery_state);
    portEXIT_CRITICAL(&s_recovery_lock);
}

static void playback_recovery_note_output(void)
{
    tater_audio_render_clock_t render_clock = {0};
    (void)tater_audio_speaker_render_clock_snapshot(&render_clock);
    uint32_t now_ms = playback_now_ms();
    portENTER_CRITICAL(&s_recovery_lock);
    tater_playback_recovery_note_output(
        &s_recovery_state,
        render_clock.completed_frames,
        now_ms
    );
    portEXIT_CRITICAL(&s_recovery_lock);
}

static void playback_recovery_stop_output(void)
{
    portENTER_CRITICAL(&s_recovery_lock);
    tater_playback_recovery_stop_output(&s_recovery_state);
    portEXIT_CRITICAL(&s_recovery_lock);
}

static bool playback_recovery_watch_rebuffer(
    bool rebuffering,
    bool recovery_allowed,
    const char *session_id
)
{
    uint32_t now_ms = playback_now_ms();
    portENTER_CRITICAL(&s_recovery_lock);
    tater_playback_recovery_action_t action =
        tater_playback_recovery_observe_rebuffer(
            &s_recovery_state,
            rebuffering,
            recovery_allowed,
            now_ms,
            PLAYBACK_RECOVERY_STALL_MS
        );
    portEXIT_CRITICAL(&s_recovery_lock);
    if (action != TATER_PLAYBACK_RECOVERY_REBUFFER_STALLED) {
        return false;
    }

    ESP_LOGW(
        TAG,
        "playback watchdog stopping stalled media rebuffer id=%s timeout_ms=%u",
        session_id && session_id[0] ? session_id : "-",
        (unsigned)PLAYBACK_RECOVERY_STALL_MS
    );
    tater_protocol_send_log(
        "warn",
        "Playback watchdog stopped a media session that could not recover from buffering."
    );
    s_abort = true;
    return true;
}

static esp_err_t playback_speaker_begin(void)
{
    return tater_audio_speaker_begin();
}

static esp_err_t playback_speaker_write(
    const int16_t *stereo_frames,
    size_t frame_count
)
{
    esp_err_t err = tater_audio_write_speaker_frames(stereo_frames, frame_count);
    if (err == ESP_OK) {
        playback_recovery_note_output();
    }
    return err;
}

static esp_err_t playback_speaker_end(void)
{
    playback_recovery_stop_output();
    return tater_audio_speaker_end();
}

static void playback_recovery_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(PLAYBACK_RECOVERY_POLL_MS));
        if (!s_playing || s_abort) {
            continue;
        }

        tater_audio_render_clock_t render_clock = {0};
        if (!tater_audio_speaker_render_clock_snapshot(&render_clock)) {
            continue;
        }
        uint32_t now_ms = playback_now_ms();
        portENTER_CRITICAL(&s_recovery_lock);
        tater_playback_recovery_action_t action =
            tater_playback_recovery_observe_output(
                &s_recovery_state,
                render_clock.completed_frames,
                now_ms,
                PLAYBACK_RECOVERY_STALL_MS
            );
        portEXIT_CRITICAL(&s_recovery_lock);
        if (action != TATER_PLAYBACK_RECOVERY_OUTPUT_STALLED) {
            continue;
        }

        ESP_LOGW(
            TAG,
            "playback watchdog stopping stalled speaker output completed_frames=%u timeout_ms=%u",
            (unsigned)render_clock.completed_frames,
            (unsigned)PLAYBACK_RECOVERY_STALL_MS
        );
        tater_protocol_send_log(
            "warn",
            "Playback watchdog stopped a stalled speaker session and reset its audio path."
        );
        s_abort = true;
    }
}

static esp_err_t playback_sink_begin(playback_pcm_sink_t *sink)
{
    if (sink && sink->begin) {
        return sink->begin(sink->ctx);
    }
    return playback_speaker_begin();
}

static esp_err_t playback_sink_write(
    playback_pcm_sink_t *sink,
    const int16_t *stereo_frames,
    size_t frame_count
)
{
    if (sink && sink->write) {
        return sink->write(sink->ctx, stereo_frames, frame_count);
    }
    return playback_speaker_write(stereo_frames, frame_count);
}

static esp_err_t playback_sink_end(playback_pcm_sink_t *sink)
{
    if (sink && sink->end) {
        return sink->end(sink->ctx);
    }
    return playback_speaker_end();
}

static uint8_t playback_sink_volume(playback_pcm_sink_t *sink)
{
    if (!sink) {
        return 100;
    }
    uint8_t volume = sink->live_volume_percent
        ? *sink->live_volume_percent
        : sink->volume_percent;
    return volume > 100 ? 100 : volume;
}

static void playback_log_heap(const char *label)
{
    ESP_LOGI(
        TAG,
        "%s heap free=%u internal=%u internal_largest=%u dma=%u dma_largest=%u psram=%u",
        label ? label : "playback",
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );
}

static BaseType_t playback_create_task(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t core,
    bool *task_with_caps
)
{
    if (task_with_caps) {
        *task_with_caps = false;
    }

#if (configSUPPORT_STATIC_ALLOCATION == 1)
    if (task_with_caps) {
        *task_with_caps = true;
    }
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        task,
        name,
        stack_depth,
        arg,
        priority,
        handle,
        core,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (ok == pdPASS) {
        return ok;
    }
    if (task_with_caps) {
        *task_with_caps = false;
    }
    ESP_LOGW(
        TAG,
        "task %s psram stack create failed stack=%u psram=%u internal=%u largest=%u; retrying internal",
        name ? name : "?",
        (unsigned)stack_depth,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
    );
#endif

    return xTaskCreatePinnedToCore(task, name, stack_depth, arg, priority, handle, core);
}

static void playback_delete_current_task(bool task_with_caps)
{
#if (configSUPPORT_STATIC_ALLOCATION == 1)
    if (task_with_caps) {
        vTaskDeleteWithCaps(NULL);
        return;
    }
#else
    (void)task_with_caps;
#endif
    vTaskDelete(NULL);
}

static bool playback_wait_stopped_locked(TickType_t timeout_ticks)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    TickType_t started = xTaskGetTickCount();

    while (true) {
        TaskHandle_t task = s_task;
        if (!s_playing) {
            s_task = NULL;
            return true;
        }
        if (!task || task == current) {
            return true;
        }

        s_abort = true;
        if (timeout_ticks != portMAX_DELAY && (xTaskGetTickCount() - started) >= timeout_ticks) {
            ESP_LOGW(TAG, "playback stop timed out task=%p playing=%d", task, s_playing);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(PLAYBACK_STOP_POLL_MS));
    }
}

static bool playback_begin_start(void)
{
    if (s_lifecycle_lock) {
        xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    }

    bool stopped = playback_wait_stopped_locked(pdMS_TO_TICKS(PLAYBACK_STOP_WAIT_MS));
    if (!stopped) {
        if (s_lifecycle_lock) {
            xSemaphoreGive(s_lifecycle_lock);
        }
        return false;
    }

    playback_recovery_begin_session();
    s_abort = false;
    s_playing = true;
    return true;
}

static void playback_end_start(void)
{
    if (s_lifecycle_lock) {
        xSemaphoreGive(s_lifecycle_lock);
    }
}

static esp_err_t playback_start_failed(esp_err_t err)
{
    playback_recovery_stop_output();
    s_task = NULL;
    s_playing = false;
    s_abort = false;
    playback_end_start();
    return err;
}

static void playback_mark_finished(void)
{
    playback_recovery_stop_output();
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (!s_task || s_task == current) {
        s_task = NULL;
        s_playing = false;
        s_abort = false;
        return;
    }

    ESP_LOGW(TAG, "stale playback task finished after a newer task started");
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool parse_wav(const uint8_t *buf, size_t len, wav_info_t *out)
{
    if (!buf || len < 44 || !out) {
        return false;
    }
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    size_t off = 12;
    bool have_fmt = false;
    bool have_data = false;
    while (off + 8 <= len) {
        const uint8_t *chunk = buf + off;
        uint32_t chunk_len = le32(chunk + 4);
        off += 8;
        if (off + chunk_len > len) {
            return false;
        }

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_len >= 16) {
            out->audio_format = le16(buf + off);
            out->channels = le16(buf + off + 2);
            out->sample_rate = le32(buf + off + 4);
            out->bits_per_sample = le16(buf + off + 14);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            out->data = buf + off;
            out->data_len = chunk_len;
            have_data = true;
        }

        off += chunk_len + (chunk_len & 1);
    }

    if (!have_fmt || !have_data || out->audio_format != 1 || out->channels == 0 || out->channels > 2) {
        return false;
    }
    if (out->bits_per_sample != 16 && out->bits_per_sample != 32) {
        return false;
    }
    size_t bytes_per_frame = ((size_t)out->bits_per_sample / 8) * out->channels;
    if (bytes_per_frame == 0) {
        return false;
    }
    out->frame_count = out->data_len / bytes_per_frame;
    return out->frame_count > 0 && out->sample_rate > 0;
}

static bool wav_stream_info_supported(const wav_stream_info_t *info)
{
    return info &&
        info->audio_format == 1 &&
        info->channels > 0 &&
        info->channels <= 2 &&
        info->sample_rate > 0 &&
        (info->bits_per_sample == 16 || info->bits_per_sample == 32) &&
        info->bytes_per_frame > 0 &&
        info->bytes_per_frame <= sizeof(((wav_stream_state_t *)0)->partial_frame);
}

static wav_header_result_t parse_wav_stream_header(const uint8_t *buf, size_t len, wav_stream_info_t *out, size_t *data_offset)
{
    if (!buf || !out || !data_offset) {
        return WAV_HEADER_INVALID;
    }
    if (len < 12) {
        return WAV_HEADER_NEED_MORE;
    }
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return WAV_HEADER_INVALID;
    }

    wav_stream_info_t info = {0};
    bool have_fmt = false;
    size_t off = 12;
    while (off + 8 <= len) {
        const uint8_t *chunk = buf + off;
        uint32_t chunk_len = le32(chunk + 4);
        size_t data_start = off + 8;

        if (memcmp(chunk, "data", 4) == 0) {
            if (!have_fmt) {
                return WAV_HEADER_INVALID;
            }
            info.data_len = chunk_len;
            info.bytes_per_frame = ((size_t)info.bits_per_sample / 8) * info.channels;
            if (!wav_stream_info_supported(&info)) {
                return WAV_HEADER_INVALID;
            }
            *out = info;
            *data_offset = data_start;
            return WAV_HEADER_OK;
        }

        if (data_start + chunk_len > len) {
            return WAV_HEADER_NEED_MORE;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_len < 16) {
                return WAV_HEADER_INVALID;
            }
            info.audio_format = le16(buf + data_start);
            info.channels = le16(buf + data_start + 2);
            info.sample_rate = le32(buf + data_start + 4);
            info.bits_per_sample = le16(buf + data_start + 14);
            have_fmt = true;
        }

        off = data_start + chunk_len + (chunk_len & 1);
    }
    return WAV_HEADER_NEED_MORE;
}

static uint8_t *alloc_audio(size_t size)
{
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buf;
}

static size_t codec_jitter_capacity(stream_audio_type_t type)
{
    return type == STREAM_AUDIO_FLAC ? PLAYBACK_FLAC_JITTER_CAPACITY : PLAYBACK_MP3_JITTER_CAPACITY;
}

static size_t codec_jitter_prebuffer(stream_audio_type_t type, int64_t content_length)
{
    size_t target = type == STREAM_AUDIO_FLAC ? PLAYBACK_FLAC_PREBUFFER : PLAYBACK_MP3_PREBUFFER;
    if (content_length > 0 && (uint64_t)content_length < target) {
        target = (size_t)content_length;
    }
    return target;
}

static size_t wav_jitter_prebuffer(int64_t content_length)
{
    size_t target = PLAYBACK_WAV_PREBUFFER_MEDIUM;
    if (content_length > 0) {
        if (content_length <= (int64_t)(160 * 1024)) {
            target = PLAYBACK_WAV_PREBUFFER_SMALL;
        } else if (content_length >= (int64_t)(512 * 1024)) {
            target = PLAYBACK_WAV_PREBUFFER_LARGE;
        }
        if ((uint64_t)content_length < target) {
            target = (size_t)content_length;
        }
    }
    if (target > PLAYBACK_WAV_JITTER_CAPACITY) {
        target = PLAYBACK_WAV_JITTER_CAPACITY;
    }
    return target;
}

static void codec_jitter_signal(codec_jitter_buffer_t *buffer)
{
    if (!buffer) {
        return;
    }
    if (buffer->can_read) {
        xSemaphoreGive(buffer->can_read);
    }
    if (buffer->can_write) {
        xSemaphoreGive(buffer->can_write);
    }
}

static esp_err_t codec_jitter_init(codec_jitter_buffer_t *buffer, size_t capacity)
{
    if (!buffer || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(buffer, 0, sizeof(*buffer));
    buffer->data = alloc_audio(capacity);
    buffer->lock = xSemaphoreCreateMutex();
    buffer->can_read = xSemaphoreCreateBinary();
    buffer->can_write = xSemaphoreCreateBinary();
    if (!buffer->data || !buffer->lock || !buffer->can_read || !buffer->can_write) {
        if (buffer->lock) {
            vSemaphoreDelete(buffer->lock);
        }
        if (buffer->can_read) {
            vSemaphoreDelete(buffer->can_read);
        }
        if (buffer->can_write) {
            vSemaphoreDelete(buffer->can_write);
        }
        free(buffer->data);
        memset(buffer, 0, sizeof(*buffer));
        return ESP_ERR_NO_MEM;
    }
    buffer->capacity = capacity;
    buffer->error = ESP_OK;
    xSemaphoreGive(buffer->can_write);
    return ESP_OK;
}

static void codec_jitter_destroy(codec_jitter_buffer_t *buffer)
{
    if (!buffer) {
        return;
    }
    if (buffer->lock) {
        vSemaphoreDelete(buffer->lock);
    }
    if (buffer->can_read) {
        vSemaphoreDelete(buffer->can_read);
    }
    if (buffer->can_write) {
        vSemaphoreDelete(buffer->can_write);
    }
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static void codec_jitter_close(codec_jitter_buffer_t *buffer)
{
    if (!buffer || !buffer->lock) {
        return;
    }
    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    buffer->eof = true;
    xSemaphoreGive(buffer->lock);
    codec_jitter_signal(buffer);
}

static void codec_jitter_fail(codec_jitter_buffer_t *buffer, esp_err_t error)
{
    if (!buffer || !buffer->lock) {
        return;
    }
    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    buffer->failed = true;
    buffer->error = error == ESP_OK ? ESP_FAIL : error;
    xSemaphoreGive(buffer->lock);
    codec_jitter_signal(buffer);
}

static size_t codec_jitter_copy_in(codec_jitter_buffer_t *buffer, const uint8_t *data, size_t len)
{
    size_t take = len;
    size_t free_space = buffer->capacity - buffer->fill;
    if (take > free_space) {
        take = free_space;
    }
    size_t first = take;
    size_t tail_space = buffer->capacity - buffer->write_pos;
    if (first > tail_space) {
        first = tail_space;
    }
    memcpy(buffer->data + buffer->write_pos, data, first);
    if (take > first) {
        memcpy(buffer->data, data + first, take - first);
    }
    buffer->write_pos = (buffer->write_pos + take) % buffer->capacity;
    buffer->fill += take;
    buffer->total_written += take;
    if (buffer->fill > buffer->high_water) {
        buffer->high_water = buffer->fill;
    }
    return take;
}

static size_t codec_jitter_copy_out(codec_jitter_buffer_t *buffer, uint8_t *out, size_t len)
{
    size_t take = len;
    if (take > buffer->fill) {
        take = buffer->fill;
    }
    size_t first = take;
    size_t tail_space = buffer->capacity - buffer->read_pos;
    if (first > tail_space) {
        first = tail_space;
    }
    memcpy(out, buffer->data + buffer->read_pos, first);
    if (take > first) {
        memcpy(out + first, buffer->data, take - first);
    }
    buffer->read_pos = (buffer->read_pos + take) % buffer->capacity;
    buffer->fill -= take;
    buffer->total_read += take;
    return take;
}

static esp_err_t codec_jitter_write(codec_jitter_buffer_t *buffer, const uint8_t *data, size_t len)
{
    if (!buffer || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = 0;
    while (!s_abort && off < len) {
        xSemaphoreTake(buffer->lock, portMAX_DELAY);
        if (buffer->failed) {
            esp_err_t err = buffer->error == ESP_OK ? ESP_FAIL : buffer->error;
            xSemaphoreGive(buffer->lock);
            return err;
        }
        size_t wrote = codec_jitter_copy_in(buffer, data + off, len - off);
        if (wrote > 0) {
            off += wrote;
            xSemaphoreGive(buffer->can_read);
        }
        xSemaphoreGive(buffer->lock);
        if (off < len) {
            xSemaphoreTake(buffer->can_write, pdMS_TO_TICKS(50));
        }
    }
    return off == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t codec_jitter_wait_prebuffer(codec_jitter_buffer_t *buffer, size_t target)
{
    if (!buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    while (!s_abort) {
        xSemaphoreTake(buffer->lock, portMAX_DELAY);
        bool failed = buffer->failed;
        bool eof = buffer->eof;
        size_t fill = buffer->fill;
        esp_err_t err = buffer->error == ESP_OK ? ESP_FAIL : buffer->error;
        xSemaphoreGive(buffer->lock);

        if (failed) {
            return err;
        }
        if (fill >= target || (eof && fill > 0)) {
            return ESP_OK;
        }
        if (eof && fill == 0) {
            return ESP_ERR_NOT_FOUND;
        }
        xSemaphoreTake(buffer->can_read, pdMS_TO_TICKS(50));
    }
    return ESP_FAIL;
}

static int codec_jitter_read(codec_jitter_buffer_t *buffer, uint8_t *out, size_t max_len, bool *eos)
{
    if (!buffer || !out || max_len == 0) {
        return -1;
    }
    if (eos) {
        *eos = false;
    }
    while (!s_abort) {
        xSemaphoreTake(buffer->lock, portMAX_DELAY);
        if (buffer->fill > 0) {
            size_t got = codec_jitter_copy_out(buffer, out, max_len);
            bool ended = buffer->eof && buffer->fill == 0;
            if (eos) {
                *eos = ended;
            }
            xSemaphoreGive(buffer->can_write);
            xSemaphoreGive(buffer->lock);
            return (int)got;
        }
        if (buffer->failed) {
            xSemaphoreGive(buffer->lock);
            return -1;
        }
        if (buffer->eof) {
            xSemaphoreGive(buffer->lock);
            return 0;
        }
        xSemaphoreGive(buffer->lock);
        xSemaphoreTake(buffer->can_read, pdMS_TO_TICKS(50));
    }
    return -1;
}

static int16_t scale_output_sample(int16_t sample, playback_pcm_sink_t *sink)
{
    const tater_live_settings_t *settings = tater_live_settings_get();
    uint32_t master_volume = sink && sink->absolute_volume
        ? 100
        : (settings ? settings->volume_percent : 100);
    uint32_t source_volume = playback_sink_volume(sink);
    uint32_t volume = (master_volume * source_volume + 50) / 100;
    if (volume >= 100) {
        return sample;
    }
    int32_t scaled = ((int32_t)sample * (int32_t)volume) / 100;
    if (scaled > INT16_MAX) {
        scaled = INT16_MAX;
    } else if (scaled < INT16_MIN) {
        scaled = INT16_MIN;
    }
    return (int16_t)scaled;
}

static int16_t wav_frame_sample_s16(const wav_stream_info_t *info, const uint8_t *frame, uint16_t channel)
{
    if (!info || !frame || info->channels == 0) {
        return 0;
    }
    if (channel >= info->channels) {
        channel = 0;
    }
    size_t bytes_per_sample = info->bits_per_sample / 8;
    const uint8_t *p = frame + ((size_t)channel * bytes_per_sample);
    if (info->bits_per_sample == 16) {
        return (int16_t)le16(p);
    }
    return (int16_t)((int32_t)le32(p) >> 16);
}

static int16_t pcm_sample_s16(uint16_t bits_per_sample, const uint8_t *sample)
{
    if (!sample) {
        return 0;
    }
    switch (bits_per_sample) {
        case 8:
            return (int16_t)(((int32_t)sample[0] - 128) << 8);
        case 16:
            return (int16_t)le16(sample);
        case 24: {
            int32_t value = (int32_t)sample[0] | ((int32_t)sample[1] << 8) | ((int32_t)sample[2] << 16);
            if (value & 0x00800000) {
                value |= 0xFF000000;
            }
            return (int16_t)(value >> 8);
        }
        case 32:
            return (int16_t)((int32_t)le32(sample) >> 16);
        default:
            return 0;
    }
}

static int16_t decoded_frame_sample_s16(const esp_audio_simple_dec_info_t *info, const uint8_t *frame, uint8_t channel)
{
    if (!info || !frame || info->channel == 0) {
        return 0;
    }
    if (channel >= info->channel) {
        channel = 0;
    }
    const size_t bytes_per_sample = info->bits_per_sample / 8;
    return pcm_sample_s16(info->bits_per_sample, frame + ((size_t)channel * bytes_per_sample));
}

static bool decoded_pcm_info_supported(const esp_audio_simple_dec_info_t *info)
{
    if (!info || info->sample_rate == 0 || info->channel == 0 || info->channel > 8) {
        return false;
    }
    if (info->bits_per_sample != 8 && info->bits_per_sample != 16 && info->bits_per_sample != 24 && info->bits_per_sample != 32) {
        return false;
    }
    size_t bytes_per_frame = ((size_t)info->bits_per_sample / 8) * info->channel;
    return bytes_per_frame > 0 && bytes_per_frame <= sizeof(((pcm_stream_state_t *)0)->partial_frame);
}

static esp_err_t pcm_stream_flush(pcm_stream_state_t *state)
{
    if (!state || state->out_frames == 0) {
        return ESP_OK;
    }
    esp_err_t err = playback_sink_write(state->sink, state->out, state->out_frames);
    if (err == ESP_OK) {
        state->output_frames += state->out_frames;
        state->out_frames = 0;
    }
    return err;
}

static esp_err_t pcm_stream_emit_stereo(uint32_t sample_rate, pcm_stream_state_t *state, int16_t left, int16_t right)
{
    if (!state || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    left = scale_output_sample(left, state->sink);
    right = scale_output_sample(right, state->sink);
    state->input_frames++;
    state->resample_accum += TATER_SPK_SAMPLE_RATE;

    while (!s_abort && state->resample_accum >= sample_rate) {
        state->out[state->out_frames * 2] = left;
        state->out[(state->out_frames * 2) + 1] = right;
        state->out_frames++;
        state->resample_accum -= sample_rate;
        if (state->out_frames >= 256) {
            esp_err_t err = pcm_stream_flush(state);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t wav_stream_emit_frame(const wav_stream_info_t *info, wav_stream_state_t *state, const uint8_t *frame)
{
    int16_t left = wav_frame_sample_s16(info, frame, 0);
    int16_t right = info->channels > 1 ? wav_frame_sample_s16(info, frame, 1) : left;
    return pcm_stream_emit_stereo(info->sample_rate, state, left, right);
}

static esp_err_t wav_stream_process_data(const wav_stream_info_t *info, wav_stream_state_t *state, const uint8_t *data, size_t len)
{
    if (!info || !state || !data || len == 0) {
        return ESP_OK;
    }

    if (info->data_len > 0 && state->data_bytes_seen >= info->data_len) {
        return ESP_OK;
    }
    if (info->data_len > 0 && state->data_bytes_seen + len > info->data_len) {
        len = info->data_len - state->data_bytes_seen;
    }

    size_t off = 0;
    while (!s_abort && off < len) {
        if (state->partial_len > 0) {
            size_t needed = info->bytes_per_frame - state->partial_len;
            size_t take = len - off < needed ? len - off : needed;
            memcpy(state->partial_frame + state->partial_len, data + off, take);
            state->partial_len += take;
            off += take;
            state->data_bytes_seen += take;
            if (state->partial_len < info->bytes_per_frame) {
                return ESP_OK;
            }
            esp_err_t err = wav_stream_emit_frame(info, state, state->partial_frame);
            state->partial_len = 0;
            if (err != ESP_OK) {
                return err;
            }
            continue;
        }

        if (len - off < info->bytes_per_frame) {
            size_t take = len - off;
            memcpy(state->partial_frame, data + off, take);
            state->partial_len = take;
            state->data_bytes_seen += take;
            return ESP_OK;
        }

        esp_err_t err = wav_stream_emit_frame(info, state, data + off);
        if (err != ESP_OK) {
            return err;
        }
        off += info->bytes_per_frame;
        state->data_bytes_seen += info->bytes_per_frame;
    }
    return ESP_OK;
}

static esp_err_t decoded_pcm_emit_frame(const esp_audio_simple_dec_info_t *info, pcm_stream_state_t *state, const uint8_t *frame)
{
    int16_t left = decoded_frame_sample_s16(info, frame, 0);
    int16_t right = info->channel > 1 ? decoded_frame_sample_s16(info, frame, 1) : left;
    return pcm_stream_emit_stereo(info->sample_rate, state, left, right);
}

static esp_err_t decoded_pcm_process_data(const esp_audio_simple_dec_info_t *info, pcm_stream_state_t *state, const uint8_t *data, size_t len)
{
    if (!decoded_pcm_info_supported(info) || !state || !data || len == 0) {
        return ESP_OK;
    }

    const size_t bytes_per_frame = ((size_t)info->bits_per_sample / 8) * info->channel;
    size_t off = 0;
    while (!s_abort && off < len) {
        if (state->partial_len > 0) {
            size_t needed = bytes_per_frame - state->partial_len;
            size_t take = len - off < needed ? len - off : needed;
            memcpy(state->partial_frame + state->partial_len, data + off, take);
            state->partial_len += take;
            off += take;
            state->data_bytes_seen += take;
            if (state->partial_len < bytes_per_frame) {
                return ESP_OK;
            }
            esp_err_t err = decoded_pcm_emit_frame(info, state, state->partial_frame);
            state->partial_len = 0;
            if (err != ESP_OK) {
                return err;
            }
            continue;
        }

        if (len - off < bytes_per_frame) {
            size_t take = len - off;
            memcpy(state->partial_frame, data + off, take);
            state->partial_len = take;
            state->data_bytes_seen += take;
            return ESP_OK;
        }

        esp_err_t err = decoded_pcm_emit_frame(info, state, data + off);
        if (err != ESP_OK) {
            return err;
        }
        off += bytes_per_frame;
        state->data_bytes_seen += bytes_per_frame;
    }
    return ESP_OK;
}

static bool url_path_has_extension(const char *url, const char *extension)
{
    if (!url || !extension) {
        return false;
    }

    const char *end = strpbrk(url, "?#");
    size_t url_len = end ? (size_t)(end - url) : strlen(url);
    const char *slash = url;
    for (size_t i = 0; i < url_len; i++) {
        if (url[i] == '/') {
            slash = url + i + 1;
        }
    }

    const char *dot = NULL;
    for (const char *p = slash; p < url + url_len; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot) {
        return false;
    }

    dot++;
    size_t ext_len = strlen(extension);
    if ((size_t)((url + url_len) - dot) != ext_len) {
        return false;
    }
    for (size_t i = 0; i < ext_len; i++) {
        if (tolower((unsigned char)dot[i]) != tolower((unsigned char)extension[i])) {
            return false;
        }
    }
    return true;
}

static stream_audio_type_t stream_audio_type_from_url(const char *url)
{
    if (url_path_has_extension(url, "wav")) {
        return STREAM_AUDIO_WAV;
    }
    if (url_path_has_extension(url, "mp3")) {
        return STREAM_AUDIO_MP3;
    }
    if (url_path_has_extension(url, "flac")) {
        return STREAM_AUDIO_FLAC;
    }
    return STREAM_AUDIO_UNKNOWN;
}

static stream_audio_type_t stream_audio_type_from_content_type(const char *content_type)
{
    if (!content_type) {
        return STREAM_AUDIO_UNKNOWN;
    }
    if (strstr(content_type, "audio/mpeg") || strstr(content_type, "audio/mp3") || strstr(content_type, "audio/x-mp3")) {
        return STREAM_AUDIO_MP3;
    }
    if (strstr(content_type, "audio/flac") || strstr(content_type, "audio/x-flac")) {
        return STREAM_AUDIO_FLAC;
    }
    if (strstr(content_type, "audio/wav") || strstr(content_type, "audio/x-wav") || strstr(content_type, "audio/wave")) {
        return STREAM_AUDIO_WAV;
    }
    return STREAM_AUDIO_UNKNOWN;
}

static stream_audio_type_t stream_audio_type_from_magic(const uint8_t *data, size_t len)
{
    if (!data) {
        return STREAM_AUDIO_UNKNOWN;
    }
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0) {
        return STREAM_AUDIO_WAV;
    }
    if (len >= 4 && memcmp(data, "fLaC", 4) == 0) {
        return STREAM_AUDIO_FLAC;
    }
    if (len >= 3 && memcmp(data, "ID3", 3) == 0) {
        return STREAM_AUDIO_MP3;
    }
    if (len >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) {
        return STREAM_AUDIO_MP3;
    }
    return STREAM_AUDIO_UNKNOWN;
}

static const char *stream_audio_type_name(stream_audio_type_t type)
{
    switch (type) {
        case STREAM_AUDIO_WAV:
            return "wav";
        case STREAM_AUDIO_MP3:
            return "mp3";
        case STREAM_AUDIO_FLAC:
            return "flac";
        default:
            return "unknown";
    }
}

static esp_audio_simple_dec_type_t simple_decoder_type_for_stream(stream_audio_type_t type)
{
    switch (type) {
        case STREAM_AUDIO_MP3:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        case STREAM_AUDIO_FLAC:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        default:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
}

static esp_err_t audio_codec_register_once(void)
{
    static bool registered;
    if (registered) {
        return ESP_OK;
    }

    esp_audio_err_t mp3_err = esp_mp3_dec_register();
    if (mp3_err != ESP_AUDIO_ERR_OK && mp3_err != ESP_AUDIO_ERR_ALREADY_EXIST) {
        ESP_LOGE(TAG, "mp3 decoder register failed err=%d", mp3_err);
        return mp3_err == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    esp_audio_err_t flac_err = esp_flac_dec_register();
    if (flac_err != ESP_AUDIO_ERR_OK && flac_err != ESP_AUDIO_ERR_ALREADY_EXIST) {
        ESP_LOGE(TAG, "flac decoder register failed err=%d", flac_err);
        return flac_err == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    registered = true;
    return ESP_OK;
}

static esp_err_t codec_stream_begin(
    codec_stream_state_t *stream,
    stream_audio_type_t type,
    playback_pcm_sink_t *sink
)
{
    if (!stream) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(audio_codec_register_once(), TAG, "codec registry failed");
    memset(stream, 0, sizeof(*stream));
    stream->type = simple_decoder_type_for_stream(type);
    stream->sink = sink;
    stream->pcm.sink = sink;
    if (stream->type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    stream->out_cap = PLAYBACK_CODEC_OUT_INITIAL;
    stream->out_buf = alloc_audio(stream->out_cap);
    if (!stream->out_buf) {
        return ESP_ERR_NO_MEM;
    }

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = stream->type,
        .use_frame_dec = false,
    };
    esp_audio_err_t err = esp_audio_simple_dec_open(&dec_cfg, &stream->decoder);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "open %s decoder failed err=%d", stream_audio_type_name(type), err);
        free(stream->out_buf);
        stream->out_buf = NULL;
        return err == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static void codec_stream_end(codec_stream_state_t *stream)
{
    if (!stream) {
        return;
    }
    if (stream->sink_started) {
        esp_err_t end_err = playback_sink_end(stream->sink);
        if (end_err != ESP_OK) {
            ESP_LOGW(TAG, "playback sink end failed err=%s", esp_err_to_name(end_err));
        }
        stream->sink_started = false;
    }
    if (stream->decoder) {
        esp_audio_simple_dec_close(stream->decoder);
        stream->decoder = NULL;
    }
    free(stream->out_buf);
    stream->out_buf = NULL;
}

static esp_err_t codec_stream_handle_pcm(codec_stream_state_t *stream, const uint8_t *data, size_t len)
{
    if (!stream || !data || len == 0) {
        return ESP_OK;
    }

    if (!stream->have_info) {
        esp_audio_err_t info_err = esp_audio_simple_dec_get_info(stream->decoder, &stream->info);
        if (info_err != ESP_AUDIO_ERR_OK || !decoded_pcm_info_supported(&stream->info)) {
            ESP_LOGE(TAG, "unsupported decoded audio info err=%d rate=%" PRIu32 " channels=%u bits=%u",
                     info_err, stream->info.sample_rate, stream->info.channel, stream->info.bits_per_sample);
            return ESP_ERR_NOT_SUPPORTED;
        }
        ESP_LOGI(TAG, "stream %s rate=%" PRIu32 " channels=%u bits=%u bitrate=%" PRIu32,
                 esp_audio_simple_dec_get_name(stream->type),
                 stream->info.sample_rate,
                 stream->info.channel,
                 stream->info.bits_per_sample,
                 stream->info.bitrate);
        ESP_RETURN_ON_ERROR(playback_sink_begin(stream->sink), TAG, "playback sink begin failed");
        stream->sink_started = true;
        stream->have_info = true;
    }

    return decoded_pcm_process_data(&stream->info, &stream->pcm, data, len);
}

static esp_err_t codec_stream_feed(codec_stream_state_t *stream, uint8_t *data, size_t len, bool eos)
{
    if (!stream || !stream->decoder || !data || len == 0) {
        return ESP_OK;
    }

    esp_audio_simple_dec_raw_t raw = {
        .buffer = data,
        .len = len,
        .eos = eos,
    };

    while (!s_abort && raw.len > 0) {
        esp_audio_simple_dec_out_t out = {
            .buffer = stream->out_buf,
            .len = stream->out_cap,
        };
        esp_audio_err_t dec_err = esp_audio_simple_dec_process(stream->decoder, &raw, &out);
        if (dec_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (out.needed_size <= stream->out_cap) {
                return ESP_FAIL;
            }
            uint8_t *new_buf = heap_caps_realloc(stream->out_buf, out.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!new_buf) {
                new_buf = heap_caps_realloc(stream->out_buf, out.needed_size, MALLOC_CAP_8BIT);
            }
            if (!new_buf) {
                return ESP_ERR_NO_MEM;
            }
            stream->out_buf = new_buf;
            stream->out_cap = out.needed_size;
            continue;
        }
        if (dec_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "decode %s failed err=%d", esp_audio_simple_dec_get_name(stream->type), dec_err);
            return ESP_FAIL;
        }

        if (out.decoded_size > 0) {
            ESP_RETURN_ON_ERROR(codec_stream_handle_pcm(stream, out.buffer, out.decoded_size), TAG, "pcm output failed");
            stream->decoded_bytes += out.decoded_size;
        }

        if (raw.consumed == 0) {
            break;
        }
        if (raw.consumed > raw.len) {
            return ESP_FAIL;
        }
        raw.len -= raw.consumed;
        raw.buffer += raw.consumed;
        raw.consumed = 0;
    }
    return ESP_OK;
}

static void codec_http_reader_task(void *arg)
{
    codec_http_reader_args_t *reader = (codec_http_reader_args_t *)arg;
    bool task_with_caps = reader ? reader->task_with_caps : false;
    esp_err_t err = ESP_OK;
    size_t bytes_seen = reader ? reader->bytes_seen : 0;

    if (!reader || !reader->client || !reader->buffer || !reader->read_buf || reader->read_size == 0) {
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }

    while (!s_abort) {
        if (reader->content_length >= 0 && (int64_t)bytes_seen >= reader->content_length) {
            break;
        }
        int got = esp_http_client_read(reader->client, (char *)reader->read_buf, reader->read_size);
        if (got < 0) {
            err = ESP_FAIL;
            break;
        }
        if (got == 0) {
            break;
        }
        bytes_seen += (size_t)got;
        err = codec_jitter_write(reader->buffer, reader->read_buf, (size_t)got);
        if (err != ESP_OK) {
            break;
        }
    }

done:
    if (reader && reader->buffer) {
        if (err == ESP_OK || s_abort) {
            codec_jitter_close(reader->buffer);
        } else {
            codec_jitter_fail(reader->buffer, err);
        }
        ESP_LOGI(
            TAG,
            "playback reader done bytes=%u err=%s",
            (unsigned)bytes_seen,
            esp_err_to_name(err == ESP_OK ? ESP_OK : err)
        );
    }
    TaskHandle_t notify_task = reader ? reader->notify_task : NULL;
    if (reader) {
        reader->done = true;
    }
    if (notify_task) {
        xTaskNotifyGive(notify_task);
    }
    playback_delete_current_task(task_with_caps);
}

static esp_err_t stream_codec_from_open_client(
    esp_http_client_handle_t client,
    stream_audio_type_t type,
    uint8_t *initial_data,
    size_t initial_len,
    uint8_t *read_buf,
    size_t read_size,
    int64_t content_length,
    playback_pcm_sink_t *sink
)
{
    codec_jitter_buffer_t jitter = {0};
    codec_stream_state_t stream = {0};
    esp_err_t err = codec_stream_begin(&stream, type, sink);
    if (err != ESP_OK) {
        return err;
    }

    size_t capacity = codec_jitter_capacity(type);
    size_t prebuffer = codec_jitter_prebuffer(type, content_length);
    err = codec_jitter_init(&jitter, capacity);
    if (err != ESP_OK) {
        codec_stream_end(&stream);
        return err;
    }
    ESP_LOGI(
        TAG,
        "stream %s jitter capacity=%u prebuffer=%u content_length=%lld",
        stream_audio_type_name(type),
        (unsigned)capacity,
        (unsigned)prebuffer,
        (long long)content_length
    );

    size_t bytes_seen = initial_len;
    err = codec_jitter_write(&jitter, initial_data, initial_len);
    TaskHandle_t reader_task = NULL;
    codec_http_reader_args_t *reader = NULL;
    if (err == ESP_OK) {
        reader = calloc(1, sizeof(*reader));
        if (!reader) {
            err = ESP_ERR_NO_MEM;
        } else {
            reader->client = client;
            reader->buffer = &jitter;
            reader->read_buf = read_buf;
            reader->read_size = read_size;
            reader->content_length = content_length;
            reader->bytes_seen = bytes_seen;
            reader->notify_task = xTaskGetCurrentTaskHandle();
            BaseType_t ok = playback_create_task(
                codec_http_reader_task,
                "codec_http",
                PLAYBACK_HTTP_READER_TASK_STACK,
                reader,
                4,
                &reader_task,
                0,
                &reader->task_with_caps
            );
            if (ok != pdPASS) {
                free(reader);
                reader = NULL;
                err = ESP_ERR_NO_MEM;
            }
        }
    }

    uint8_t *decode_buf = NULL;
    if (err == ESP_OK) {
        err = codec_jitter_wait_prebuffer(&jitter, prebuffer);
    }
    if (err == ESP_OK) {
        decode_buf = alloc_audio(read_size);
        if (!decode_buf) {
            err = ESP_ERR_NO_MEM;
        }
    }
    while (!s_abort && err == ESP_OK) {
        bool eos = false;
        int got = codec_jitter_read(&jitter, decode_buf, read_size, &eos);
        if (got < 0) {
            err = jitter.error == ESP_OK ? ESP_FAIL : jitter.error;
            break;
        }
        if (got == 0) {
            break;
        }
        err = codec_stream_feed(&stream, decode_buf, (size_t)got, eos);
    }

    if (err == ESP_OK && !s_abort) {
        if (stream.sink_started) {
            err = pcm_stream_flush(&stream.pcm);
        } else {
            err = ESP_ERR_NOT_SUPPORTED;
        }
    }
    ESP_LOGI(
        TAG,
        "stream %s playback input_frames=%u output_frames=%u decoded_bytes=%u",
        stream_audio_type_name(type),
        (unsigned)stream.pcm.input_frames,
        (unsigned)stream.pcm.output_frames,
        (unsigned)stream.decoded_bytes
    );
    free(decode_buf);
    codec_stream_end(&stream);
    if (err == ESP_OK && !s_abort) {
        codec_jitter_close(&jitter);
    } else {
        codec_jitter_fail(&jitter, err == ESP_OK ? ESP_FAIL : err);
    }
    if (reader_task) {
        uint32_t waited_ms = 0;
        while (reader && !reader->done) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            waited_ms += 100;
            if (waited_ms == 6000 || (waited_ms > 6000 && (waited_ms % 5000) == 0)) {
                ESP_LOGW(TAG, "codec reader still stopping after %u ms", (unsigned)waited_ms);
            }
        }
    }
    free(reader);
    ESP_LOGI(
        TAG,
        "stream %s jitter read=%u wrote=%u high_water=%u",
        stream_audio_type_name(type),
        (unsigned)jitter.total_read,
        (unsigned)jitter.total_written,
        (unsigned)jitter.high_water
    );
    codec_jitter_destroy(&jitter);
    if (err != ESP_OK) {
        return err;
    }
    return s_abort ? ESP_FAIL : ESP_OK;
}

static esp_err_t stream_wav_direct_from_open_client(
    esp_http_client_handle_t client,
    const wav_stream_info_t *info,
    const uint8_t *initial_audio,
    size_t initial_audio_len,
    uint8_t *read_buf,
    size_t read_size,
    const char *reason,
    playback_pcm_sink_t *sink
)
{
    if (!client || !info || !read_buf || read_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (reason && reason[0]) {
        tater_protocol_send_log("warn", reason);
    }

    wav_stream_state_t state = {
        .sink = sink,
    };
    esp_err_t err = playback_sink_begin(sink);
    bool speaker_started = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "playback sink begin failed: %s", esp_err_to_name(err));
        return err;
    }

    if (initial_audio_len > 0) {
        err = wav_stream_process_data(info, &state, initial_audio, initial_audio_len);
    }
    while (!s_abort && err == ESP_OK) {
        if (info->data_len > 0 && state.data_bytes_seen >= info->data_len) {
            break;
        }
        int got = esp_http_client_read(client, (char *)read_buf, read_size);
        if (got < 0) {
            err = ESP_FAIL;
            break;
        }
        if (got == 0) {
            break;
        }
        err = wav_stream_process_data(info, &state, read_buf, (size_t)got);
    }

    if (err == ESP_OK && !s_abort) {
        err = pcm_stream_flush(&state);
    }
    ESP_LOGI(
        TAG,
        "stream wav direct playback input_frames=%u output_frames=%u bytes=%u",
        (unsigned)state.input_frames,
        (unsigned)state.output_frames,
        (unsigned)state.data_bytes_seen
    );
    if (speaker_started) {
        esp_err_t end_err = playback_sink_end(sink);
        if (end_err != ESP_OK) {
            ESP_LOGW(TAG, "playback sink end failed err=%s", esp_err_to_name(end_err));
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    return s_abort ? ESP_FAIL : ESP_OK;
}

static esp_err_t stream_wav_from_open_client(
    esp_http_client_handle_t client,
    const wav_stream_info_t *info,
    uint8_t *initial_audio,
    size_t initial_audio_len,
    size_t http_bytes_seen,
    uint8_t *read_buf,
    size_t read_size,
    int64_t content_length,
    playback_pcm_sink_t *sink
)
{
    if (!client || !info || !read_buf || read_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    codec_jitter_buffer_t jitter = {0};
    wav_stream_state_t state = {
        .sink = sink,
    };
    esp_err_t err = codec_jitter_init(&jitter, PLAYBACK_WAV_JITTER_CAPACITY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream wav jitter init failed err=%s; using direct playback", esp_err_to_name(err));
        return stream_wav_direct_from_open_client(
            client,
            info,
            initial_audio,
            initial_audio_len,
            read_buf,
            read_size,
            "wav buffered playback unavailable; using direct playback",
            sink
        );
    }

    size_t prebuffer = wav_jitter_prebuffer(content_length);
    ESP_LOGI(
        TAG,
        "stream wav jitter capacity=%u prebuffer=%u content_length=%lld initial=%u",
        (unsigned)PLAYBACK_WAV_JITTER_CAPACITY,
        (unsigned)prebuffer,
        (long long)content_length,
        (unsigned)initial_audio_len
    );

    if (initial_audio_len > 0) {
        err = codec_jitter_write(&jitter, initial_audio, initial_audio_len);
    }

    TaskHandle_t reader_task = NULL;
    codec_http_reader_args_t *reader = NULL;
    if (err == ESP_OK) {
        reader = calloc(1, sizeof(*reader));
        if (!reader) {
            ESP_LOGW(TAG, "stream wav reader alloc failed; using direct playback");
            err = ESP_ERR_NO_MEM;
        } else {
            reader->client = client;
            reader->buffer = &jitter;
            reader->read_buf = read_buf;
            reader->read_size = read_size;
            reader->content_length = content_length;
            reader->bytes_seen = http_bytes_seen;
            reader->notify_task = xTaskGetCurrentTaskHandle();
            BaseType_t ok = playback_create_task(
                codec_http_reader_task,
                "wav_http",
                PLAYBACK_HTTP_READER_TASK_STACK,
                reader,
                4,
                &reader_task,
                0,
                &reader->task_with_caps
            );
            if (ok != pdPASS) {
                ESP_LOGW(
                    TAG,
                    "stream wav reader task create failed stack=%u internal=%u largest=%u; using direct playback",
                    (unsigned)PLAYBACK_HTTP_READER_TASK_STACK,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
                );
                free(reader);
                reader = NULL;
                err = ESP_ERR_NO_MEM;
            }
        }
    }
    if (err != ESP_OK && !reader_task) {
        codec_jitter_destroy(&jitter);
        return stream_wav_direct_from_open_client(
            client,
            info,
            initial_audio,
            initial_audio_len,
            read_buf,
            read_size,
            "wav reader unavailable; using direct playback",
            sink
        );
    }

    uint8_t *decode_buf = NULL;
    bool speaker_started = false;
    if (err == ESP_OK) {
        err = codec_jitter_wait_prebuffer(&jitter, prebuffer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "stream wav prebuffer failed err=%s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        decode_buf = alloc_audio(read_size);
        if (!decode_buf) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        tater_protocol_send_log("info", "wav buffered playback");
        err = playback_sink_begin(sink);
        if (err == ESP_OK) {
            speaker_started = true;
        } else {
            ESP_LOGE(TAG, "playback sink begin failed: %s", esp_err_to_name(err));
        }
    }

    while (!s_abort && err == ESP_OK) {
        bool eos = false;
        int got = codec_jitter_read(&jitter, decode_buf, read_size, &eos);
        if (got < 0) {
            err = jitter.error == ESP_OK ? ESP_FAIL : jitter.error;
            break;
        }
        if (got == 0) {
            break;
        }
        err = wav_stream_process_data(info, &state, decode_buf, (size_t)got);
        if (err != ESP_OK || eos) {
            break;
        }
    }

    if (err == ESP_OK && !s_abort && speaker_started) {
        err = pcm_stream_flush(&state);
    }
    ESP_LOGI(
        TAG,
        "stream wav playback input_frames=%u output_frames=%u bytes=%u",
        (unsigned)state.input_frames,
        (unsigned)state.output_frames,
        (unsigned)state.data_bytes_seen
    );
    if (speaker_started) {
        esp_err_t end_err = playback_sink_end(sink);
        if (end_err != ESP_OK) {
            ESP_LOGW(TAG, "playback sink end failed err=%s", esp_err_to_name(end_err));
        }
    }

    free(decode_buf);
    if (err == ESP_OK && !s_abort) {
        codec_jitter_close(&jitter);
    } else {
        codec_jitter_fail(&jitter, err == ESP_OK ? ESP_FAIL : err);
    }
    if (reader_task) {
        uint32_t waited_ms = 0;
        while (reader && !reader->done) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            waited_ms += 100;
            if (waited_ms == 6000 || (waited_ms > 6000 && (waited_ms % 5000) == 0)) {
                ESP_LOGW(TAG, "wav reader still stopping after %u ms", (unsigned)waited_ms);
            }
        }
    }
    free(reader);
    ESP_LOGI(
        TAG,
        "stream wav jitter read=%u wrote=%u high_water=%u",
        (unsigned)jitter.total_read,
        (unsigned)jitter.total_written,
        (unsigned)jitter.high_water
    );
    codec_jitter_destroy(&jitter);
    if (err != ESP_OK) {
        return err;
    }
    return s_abort ? ESP_FAIL : ESP_OK;
}

static esp_err_t stream_audio_url(const char *url, playback_pcm_sink_t *sink)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = PLAYBACK_HTTP_READ_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "http client init failed");

    uint8_t *read_buf = alloc_audio(PLAYBACK_HTTP_READ_SIZE);
    uint8_t *header_buf = alloc_audio(PLAYBACK_HEADER_LIMIT);
    if (!read_buf || !header_buf) {
        if (read_buf) {
            free(read_buf);
        }
        if (header_buf) {
            free(header_buf);
        }
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        goto done;
    }

    (void)esp_http_client_fetch_headers(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    stream_audio_type_t hint_type = stream_audio_type_from_url(url);
    char *content_type = NULL;
    if (esp_http_client_get_header(client, "Content-Type", &content_type) == ESP_OK && content_type) {
        stream_audio_type_t content_type_hint = stream_audio_type_from_content_type(content_type);
        if (hint_type == STREAM_AUDIO_UNKNOWN) {
            hint_type = content_type_hint;
        }
        ESP_LOGI(TAG, "playback content-type=%s hint=%s", content_type, stream_audio_type_name(hint_type));
    }
    wav_stream_info_t info = {0};
    size_t header_len = 0;
    bool header_ready = false;

    while (!s_abort) {
        int got = esp_http_client_read(client, (char *)read_buf, PLAYBACK_HTTP_READ_SIZE);
        if (got < 0) {
            err = ESP_FAIL;
            goto done;
        }
        if (got == 0) {
            break;
        }

        if (!header_ready) {
            if (header_len + (size_t)got > PLAYBACK_HEADER_LIMIT) {
                ESP_LOGE(TAG, "audio header exceeded limit");
                err = ESP_ERR_NOT_SUPPORTED;
                goto done;
            }
            memcpy(header_buf + header_len, read_buf, (size_t)got);
            header_len += (size_t)got;

            stream_audio_type_t magic_type = stream_audio_type_from_magic(header_buf, header_len);
            stream_audio_type_t stream_type = magic_type != STREAM_AUDIO_UNKNOWN ? magic_type : hint_type;
            if (stream_type == STREAM_AUDIO_MP3 || stream_type == STREAM_AUDIO_FLAC) {
                ESP_LOGI(TAG, "stream audio format=%s", stream_audio_type_name(stream_type));
                err = stream_codec_from_open_client(
                    client,
                    stream_type,
                    header_buf,
                    header_len,
                    read_buf,
                    PLAYBACK_HTTP_READ_SIZE,
                    content_length,
                    sink
                );
                goto done;
            }

            size_t data_offset = 0;
            wav_header_result_t header = parse_wav_stream_header(header_buf, header_len, &info, &data_offset);
            if (header == WAV_HEADER_NEED_MORE) {
                continue;
            }
            if (header == WAV_HEADER_INVALID) {
                ESP_LOGE(TAG, "unsupported streamed audio");
                err = ESP_ERR_NOT_SUPPORTED;
                goto done;
            }

            ESP_LOGI(
                TAG,
                "stream wav rate=%" PRIu32 " channels=%u bits=%u data_bytes=%" PRIu32,
                info.sample_rate,
                info.channels,
                info.bits_per_sample,
                info.data_len
            );
            header_ready = true;
            err = stream_wav_from_open_client(
                client,
                &info,
                header_buf + data_offset,
                header_len - data_offset,
                header_len,
                read_buf,
                PLAYBACK_HTTP_READ_SIZE,
                content_length,
                sink
            );
            goto done;
        }
    }

    if (!header_ready) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }

done:
    esp_http_client_cleanup(client);
    free(read_buf);
    free(header_buf);
    if (err != ESP_OK) {
        return err;
    }
    return s_abort ? ESP_FAIL : ESP_OK;
}

static void scene_pcm_ring_signal(scene_pcm_ring_t *ring)
{
    if (!ring) {
        return;
    }
    if (ring->can_read) {
        xSemaphoreGive(ring->can_read);
    }
    if (ring->can_write) {
        xSemaphoreGive(ring->can_write);
    }
}

static esp_err_t scene_pcm_ring_init(scene_pcm_ring_t *ring, size_t capacity_frames)
{
    if (!ring || capacity_frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ring, 0, sizeof(*ring));
    ring->samples = (int16_t *)alloc_audio(
        capacity_frames * TATER_SPK_CHANNELS * sizeof(int16_t)
    );
    ring->lock = xSemaphoreCreateMutex();
    ring->can_read = xSemaphoreCreateBinary();
    ring->can_write = xSemaphoreCreateBinary();
    if (!ring->samples || !ring->lock || !ring->can_read || !ring->can_write) {
        if (ring->lock) {
            vSemaphoreDelete(ring->lock);
        }
        if (ring->can_read) {
            vSemaphoreDelete(ring->can_read);
        }
        if (ring->can_write) {
            vSemaphoreDelete(ring->can_write);
        }
        free(ring->samples);
        memset(ring, 0, sizeof(*ring));
        return ESP_ERR_NO_MEM;
    }
    ring->capacity_frames = capacity_frames;
    xSemaphoreGive(ring->can_write);
    return ESP_OK;
}

static void scene_pcm_ring_destroy(scene_pcm_ring_t *ring)
{
    if (!ring) {
        return;
    }
    if (ring->lock) {
        vSemaphoreDelete(ring->lock);
    }
    if (ring->can_read) {
        vSemaphoreDelete(ring->can_read);
    }
    if (ring->can_write) {
        vSemaphoreDelete(ring->can_write);
    }
    free(ring->samples);
    memset(ring, 0, sizeof(*ring));
}

static void scene_pcm_ring_stop(scene_pcm_ring_t *ring)
{
    if (!ring || !ring->lock) {
        return;
    }
    xSemaphoreTake(ring->lock, portMAX_DELAY);
    ring->stop = true;
    xSemaphoreGive(ring->lock);
    scene_pcm_ring_signal(ring);
}

static void scene_pcm_ring_finish(scene_pcm_ring_t *ring, esp_err_t result)
{
    if (!ring || !ring->lock) {
        return;
    }
    xSemaphoreTake(ring->lock, portMAX_DELAY);
    ring->closed = true;
    if (result != ESP_OK && !ring->stop && !s_abort) {
        ring->failed = true;
    }
    xSemaphoreGive(ring->lock);
    scene_pcm_ring_signal(ring);
}

static esp_err_t scene_pcm_ring_write(
    void *ctx,
    const int16_t *stereo_frames,
    size_t frame_count
)
{
    scene_pcm_ring_t *ring = (scene_pcm_ring_t *)ctx;
    if (!ring || !ring->lock || !stereo_frames || frame_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    while (!s_abort && offset < frame_count) {
        xSemaphoreTake(ring->lock, portMAX_DELAY);
        if (ring->stop) {
            xSemaphoreGive(ring->lock);
            return ESP_ERR_INVALID_STATE;
        }

        size_t available = ring->capacity_frames - ring->fill_frames;
        size_t write_frames = frame_count - offset;
        if (write_frames > available) {
            write_frames = available;
        }
        size_t contiguous = ring->capacity_frames - ring->write_frame;
        if (write_frames > contiguous) {
            write_frames = contiguous;
        }
        if (write_frames > 0) {
            memcpy(
                &ring->samples[ring->write_frame * TATER_SPK_CHANNELS],
                &stereo_frames[offset * TATER_SPK_CHANNELS],
                write_frames * TATER_SPK_CHANNELS * sizeof(int16_t)
            );
            ring->write_frame = (ring->write_frame + write_frames) % ring->capacity_frames;
            ring->fill_frames += write_frames;
            ring->total_written_frames += write_frames;
            if (ring->fill_frames > ring->high_water_frames) {
                ring->high_water_frames = ring->fill_frames;
            }
            offset += write_frames;
            xSemaphoreGive(ring->can_read);
        }
        xSemaphoreGive(ring->lock);

        if (offset < frame_count) {
            xSemaphoreTake(ring->can_write, pdMS_TO_TICKS(20));
        }
    }
    return offset == frame_count ? ESP_OK : ESP_FAIL;
}

static size_t scene_pcm_ring_read(
    scene_pcm_ring_t *ring,
    int16_t *stereo_frames,
    size_t frame_count,
    TickType_t wait_ticks
)
{
    if (!ring || !ring->lock || !stereo_frames || frame_count == 0) {
        return 0;
    }

    TickType_t started = xTaskGetTickCount();
    while (!s_abort) {
        xSemaphoreTake(ring->lock, portMAX_DELAY);
        size_t read_frames = frame_count;
        if (read_frames > ring->fill_frames) {
            read_frames = ring->fill_frames;
        }
        if (read_frames > 0) {
            size_t first = read_frames;
            size_t contiguous = ring->capacity_frames - ring->read_frame;
            if (first > contiguous) {
                first = contiguous;
            }
            memcpy(
                stereo_frames,
                &ring->samples[ring->read_frame * TATER_SPK_CHANNELS],
                first * TATER_SPK_CHANNELS * sizeof(int16_t)
            );
            if (read_frames > first) {
                memcpy(
                    &stereo_frames[first * TATER_SPK_CHANNELS],
                    ring->samples,
                    (read_frames - first) * TATER_SPK_CHANNELS * sizeof(int16_t)
                );
            }
            ring->read_frame = (ring->read_frame + read_frames) % ring->capacity_frames;
            ring->fill_frames -= read_frames;
            ring->total_read_frames += read_frames;
            if (read_frames < frame_count) {
                ring->underrun_frames += frame_count - read_frames;
            }
            xSemaphoreGive(ring->can_write);
            xSemaphoreGive(ring->lock);
            return read_frames;
        }
        bool ended = ring->closed || ring->failed || ring->stop;
        xSemaphoreGive(ring->lock);

        if (ended || wait_ticks == 0 || (xTaskGetTickCount() - started) >= wait_ticks) {
            xSemaphoreTake(ring->lock, portMAX_DELAY);
            ring->underrun_frames += frame_count;
            xSemaphoreGive(ring->lock);
            return 0;
        }
        xSemaphoreTake(ring->can_read, pdMS_TO_TICKS(10));
    }
    return 0;
}

static bool scene_pcm_ring_wait_prebuffer(
    scene_pcm_ring_t *ring,
    uint32_t timeout_ms
)
{
    if (!ring || !ring->lock) {
        return false;
    }

    const size_t target_frames =
        ((size_t)TATER_SPK_SAMPLE_RATE * PLAYBACK_SCENE_PREBUFFER_MS) / 1000;
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(
        timeout_ms > 0 ? timeout_ms : PLAYBACK_SCENE_PREBUFFER_TIMEOUT_MS
    );
    while (!s_abort) {
        xSemaphoreTake(ring->lock, portMAX_DELAY);
        size_t fill_frames = ring->fill_frames;
        bool ended = ring->closed || ring->failed;
        xSemaphoreGive(ring->lock);

        if (fill_frames >= target_frames || (ended && fill_frames > 0)) {
            return true;
        }
        if (ended || (xTaskGetTickCount() - started) >= timeout) {
            return false;
        }
        xSemaphoreTake(ring->can_read, pdMS_TO_TICKS(50));
    }
    return false;
}

static void scene_pcm_ring_snapshot(
    scene_pcm_ring_t *ring,
    size_t *fill_frames,
    bool *ended
)
{
    if (fill_frames) {
        *fill_frames = 0;
    }
    if (ended) {
        *ended = true;
    }
    if (!ring || !ring->lock) {
        return;
    }

    xSemaphoreTake(ring->lock, portMAX_DELAY);
    if (fill_frames) {
        *fill_frames = ring->fill_frames;
    }
    if (ended) {
        *ended = ring->closed || ring->failed || ring->stop;
    }
    xSemaphoreGive(ring->lock);
}

static size_t scene_pcm_ring_discard(scene_pcm_ring_t *ring, size_t frame_count)
{
    if (!ring || !ring->lock || frame_count == 0) {
        return 0;
    }
    xSemaphoreTake(ring->lock, portMAX_DELAY);
    size_t discarded = frame_count < ring->fill_frames ? frame_count : ring->fill_frames;
    if (discarded > 0) {
        ring->read_frame = (ring->read_frame + discarded) % ring->capacity_frames;
        ring->fill_frames -= discarded;
        ring->total_read_frames += discarded;
        xSemaphoreGive(ring->can_write);
    }
    xSemaphoreGive(ring->lock);
    return discarded;
}

static esp_err_t scene_background_sink_begin(void *ctx)
{
    scene_background_args_t *request = (scene_background_args_t *)ctx;
    return request && request->ring && !request->ring->stop
        ? ESP_OK
        : ESP_ERR_INVALID_STATE;
}

static esp_err_t scene_background_sink_end(void *ctx)
{
    (void)ctx;
    return ESP_OK;
}

static esp_err_t scene_background_sink_write(
    void *ctx,
    const int16_t *stereo_frames,
    size_t frame_count
)
{
    scene_background_args_t *request = (scene_background_args_t *)ctx;
    if (!request || !request->ring || !stereo_frames || frame_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t skip = request->skip_frames_remaining > frame_count
        ? frame_count
        : (size_t)request->skip_frames_remaining;
    request->skip_frames_remaining -= skip;
    request->skipped_frames += skip;
    if (skip >= frame_count) {
        return ESP_OK;
    }
    return scene_pcm_ring_write(
        request->ring,
        stereo_frames + (skip * TATER_SPK_CHANNELS),
        frame_count - skip
    );
}

static void scene_background_task(void *arg)
{
    scene_background_args_t *request = (scene_background_args_t *)arg;
    bool task_with_caps = request ? request->task_with_caps : false;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (request && request->ring && request->url && request->url[0]) {
        playback_pcm_sink_t sink = {
            .ctx = request,
            .begin = scene_background_sink_begin,
            .write = scene_background_sink_write,
            .end = scene_background_sink_end,
            .volume_percent = request->volume_percent,
            .live_volume_percent = request->live_volume_percent,
            .absolute_volume = request->absolute_volume,
        };
        do {
            err = stream_audio_url(request->url, &sink);
            if (err != ESP_OK || !request->loop || request->ring->stop || s_abort) {
                break;
            }
            ESP_LOGI(TAG, "audio scene background loop restarting");
        } while (!s_abort && !request->ring->stop);
    }

    if (request && request->ring) {
        scene_pcm_ring_finish(request->ring, err);
    }
    if (request) {
        request->result = err;
        request->done = true;
        if (request->notify_task) {
            xTaskNotifyGive(request->notify_task);
        }
    }
    playback_delete_current_task(task_with_caps);
}

static esp_err_t scene_mixer_sink_begin(void *ctx)
{
    scene_mixer_sink_t *mixer = (scene_mixer_sink_t *)ctx;
    if (!mixer) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = playback_speaker_begin();
    if (err != ESP_OK) {
        return err;
    }
    mixer->speaker_started = true;
    tater_playback_mix_init(&mixer->mix, mixer->background ? 0 : 100);
    if (mixer->background) {
        uint32_t attack_frames =
            ((uint32_t)TATER_SPK_SAMPLE_RATE * mixer->ducking_attack_ms) / 1000;
        tater_playback_mix_set_background(
            &mixer->mix,
            mixer->ducking_target_percent,
            attack_frames
        );
    }
    return ESP_OK;
}

static esp_err_t scene_mixer_sink_write(
    void *ctx,
    const int16_t *foreground_stereo,
    size_t frame_count
)
{
    scene_mixer_sink_t *mixer = (scene_mixer_sink_t *)ctx;
    if (!mixer || !foreground_stereo || frame_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t background[256 * TATER_SPK_CHANNELS];
    int16_t mixed[256 * TATER_SPK_CHANNELS];
    size_t offset = 0;
    while (offset < frame_count) {
        size_t frames = frame_count - offset;
        if (frames > 256) {
            frames = 256;
        }
        memset(background, 0, frames * TATER_SPK_CHANNELS * sizeof(int16_t));
        if (mixer->background) {
            (void)scene_pcm_ring_read(mixer->background, background, frames, 0);
        }
        tater_playback_mix_frames(
            &mixer->mix,
            &foreground_stereo[offset * TATER_SPK_CHANNELS],
            mixer->background ? background : NULL,
            mixed,
            frames
        );
        esp_err_t err = playback_speaker_write(mixed, frames);
        if (err != ESP_OK) {
            return err;
        }
        offset += frames;
    }
    return ESP_OK;
}

static esp_err_t scene_mixer_sink_end(void *ctx)
{
    scene_mixer_sink_t *mixer = (scene_mixer_sink_t *)ctx;
    if (!mixer || !mixer->speaker_started) {
        return ESP_OK;
    }

    uint16_t fade_ms = mixer->fade_out_ms > 0
        ? mixer->fade_out_ms
        : mixer->ducking_release_ms;
    if (mixer->background && fade_ms > 0 && !s_abort) {
        uint32_t fade_frames = ((uint32_t)TATER_SPK_SAMPLE_RATE * fade_ms) / 1000;
        tater_playback_mix_set_background(&mixer->mix, 0, fade_frames);
        int16_t foreground[256 * TATER_SPK_CHANNELS] = {0};
        int16_t background[256 * TATER_SPK_CHANNELS];
        int16_t mixed[256 * TATER_SPK_CHANNELS];
        uint32_t remaining = fade_frames;
        while (!s_abort && remaining > 0) {
            size_t frames = remaining > 256 ? 256 : remaining;
            memset(background, 0, frames * TATER_SPK_CHANNELS * sizeof(int16_t));
            size_t got = scene_pcm_ring_read(
                mixer->background,
                background,
                frames,
                pdMS_TO_TICKS(30)
            );
            if (got == 0) {
                break;
            }
            tater_playback_mix_frames(
                &mixer->mix,
                foreground,
                background,
                mixed,
                got
            );
            esp_err_t err = playback_speaker_write(mixed, got);
            if (err != ESP_OK) {
                break;
            }
            remaining -= got;
        }
    }

    mixer->speaker_started = false;
    return playback_speaker_end();
}

static int16_t wav_sample_s16(const wav_info_t *wav, size_t frame, uint16_t channel)
{
    if (frame >= wav->frame_count) {
        return 0;
    }
    if (channel >= wav->channels) {
        channel = 0;
    }
    size_t bytes_per_sample = wav->bits_per_sample / 8;
    size_t index = (frame * wav->channels + channel) * bytes_per_sample;
    const uint8_t *p = wav->data + index;
    if (wav->bits_per_sample == 16) {
        return (int16_t)le16(p);
    }
    return (int16_t)((int32_t)le32(p) >> 16);
}

static esp_err_t play_wav(const wav_info_t *wav)
{
    if (!tater_audio_speaker_ready()) {
        ESP_LOGW(TAG, "speaker path is not ready");
    }

    ESP_RETURN_ON_ERROR(playback_speaker_begin(), TAG, "speaker begin failed");
    int16_t out[256 * TATER_SPK_CHANNELS];
    uint32_t played_frames = 0;
    esp_err_t result = ESP_OK;
    uint64_t pos_q32 = 0;
    uint64_t step_q32 = ((uint64_t)wav->sample_rate << 32) / TATER_SPK_SAMPLE_RATE;
    if (step_q32 == 0) {
        step_q32 = 1;
    }

    while (!s_abort) {
        size_t frames = 0;
        while (frames < 256) {
            size_t src_frame = (size_t)(pos_q32 >> 32);
            if (src_frame >= wav->frame_count) {
                break;
            }
            int16_t left = wav_sample_s16(wav, src_frame, 0);
            int16_t right = wav->channels > 1 ? wav_sample_s16(wav, src_frame, 1) : left;
            out[frames * 2] = scale_output_sample(left, NULL);
            out[(frames * 2) + 1] = scale_output_sample(right, NULL);
            frames++;
            pos_q32 += step_q32;
        }
        if (frames == 0) {
            break;
        }
        esp_err_t err = playback_speaker_write(out, frames);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "speaker write failed err=%s", esp_err_to_name(err));
            result = err;
            break;
        }
        played_frames += frames;
    }
    ESP_LOGI(TAG, "wav playback wrote frames=%u", (unsigned)played_frames);
    esp_err_t end_err = playback_speaker_end();
    if (end_err != ESP_OK) {
        ESP_LOGW(TAG, "speaker end failed err=%s", esp_err_to_name(end_err));
    }
    if (result != ESP_OK) {
        return result;
    }
    return s_abort ? ESP_FAIL : ESP_OK;
}

static int16_t triangle_sample(uint32_t frame, uint32_t period_frames, int16_t amplitude)
{
    if (period_frames < 2) {
        return 0;
    }
    uint32_t pos = frame % period_frames;
    uint32_t half = period_frames / 2;
    if (half == 0) {
        return 0;
    }
    if (pos < half) {
        int32_t value = -(int32_t)amplitude + ((int32_t)(2 * amplitude) * (int32_t)pos) / (int32_t)half;
        return (int16_t)value;
    }
    uint32_t down_len = period_frames - half;
    if (down_len == 0) {
        return 0;
    }
    int32_t value = (int32_t)amplitude - ((int32_t)(2 * amplitude) * (int32_t)(pos - half)) / (int32_t)down_len;
    return (int16_t)value;
}

static esp_err_t play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent)
{
    if (frequency_hz < 80) {
        frequency_hz = 80;
    } else if (frequency_hz > 8000) {
        frequency_hz = 8000;
    }
    if (duration_ms < 100) {
        duration_ms = 100;
    } else if (duration_ms > 10000) {
        duration_ms = 10000;
    }
    if (volume_percent > 100) {
        volume_percent = 100;
    }

    ESP_RETURN_ON_ERROR(playback_speaker_begin(), TAG, "speaker begin failed");

    const uint32_t total_frames = (TATER_SPK_SAMPLE_RATE * duration_ms) / 1000;
    const uint32_t period_frames = TATER_SPK_SAMPLE_RATE / frequency_hz;
    const int16_t amplitude = (int16_t)((12000 * volume_percent) / 100);
    int16_t out[256 * TATER_SPK_CHANNELS];
    uint32_t played_frames = 0;
    esp_err_t result = ESP_OK;

    while (!s_abort && played_frames < total_frames) {
        size_t frames = total_frames - played_frames;
        if (frames > 256) {
            frames = 256;
        }
        for (size_t i = 0; i < frames; i++) {
            int16_t sample = triangle_sample(played_frames + i, period_frames, amplitude);
            out[i * 2] = sample;
            out[(i * 2) + 1] = sample;
        }
        esp_err_t err = playback_speaker_write(out, frames);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tone speaker write failed err=%s", esp_err_to_name(err));
            result = err;
            break;
        }
        played_frames += frames;
    }

    ESP_LOGI(
        TAG,
        "tone playback wrote frames=%u frequency=%" PRIu32 " duration_ms=%" PRIu32 " volume_percent=%u",
        (unsigned)played_frames,
        frequency_hz,
        duration_ms,
        volume_percent
    );
    esp_err_t end_err = playback_speaker_end();
    if (end_err != ESP_OK) {
        ESP_LOGW(TAG, "speaker end failed err=%s", esp_err_to_name(end_err));
    }
    if (result != ESP_OK) {
        return result;
    }
    return s_abort ? ESP_FAIL : ESP_OK;
}

static void playback_task(void *arg)
{
    playback_args_t *request = (playback_args_t *)arg;
    char *url = request ? request->url : NULL;
    bool notify_finished = request ? request->notify_finished : true;
    bool task_with_caps = request ? request->task_with_caps : false;

    s_playing = true;
    ESP_LOGI(TAG, "playback url=%s", url);
    tater_protocol_send_log("info", "playback started");

    esp_err_t err = stream_audio_url(url, NULL);
    bool aborted = s_abort;
    playback_mark_finished();
    if (notify_finished) {
        if (!aborted && err == ESP_OK) {
            tater_protocol_send_playback_finished();
            tater_protocol_send_log("info", "playback finished");
        } else {
            tater_protocol_send_playback_finished_status(false, false);
            tater_protocol_send_log("warn", "playback stopped or failed");
        }
    } else if (!aborted && err == ESP_OK) {
        tater_protocol_send_log("info", "local playback finished");
    } else {
        tater_protocol_send_log("warn", "local playback stopped or failed");
    }
    free(url);
    free(request);
    playback_delete_current_task(task_with_caps);
}

static void scene_task(void *arg)
{
    scene_args_t *request = (scene_args_t *)arg;
    bool task_with_caps = request ? request->task_with_caps : false;
    scene_pcm_ring_t background_ring = {0};
    scene_background_args_t *background_request = NULL;
    TaskHandle_t background_task = NULL;
    bool background_ready = false;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    s_playing = true;
    ESP_LOGI(
        TAG,
        "audio scene started id=%s foreground=%s background=%s loop=%d duck=%u%% attack=%ums release=%ums fade=%ums",
        request ? request->scene_id : "",
        request && request->foreground_url ? request->foreground_url : "",
        request && request->background_url ? request->background_url : "-",
        request ? request->background_loop : false,
        request ? request->ducking_target_percent : 0,
        request ? request->ducking_attack_ms : 0,
        request ? request->ducking_release_ms : 0,
        request ? request->background_fade_out_ms : 0
    );
    tater_protocol_send_log("info", "audio scene started");

    if (!request || !request->foreground_url || !request->foreground_url[0]) {
        goto done;
    }

    if (request->background_url && request->background_url[0]) {
        esp_err_t ring_err = scene_pcm_ring_init(
            &background_ring,
            PLAYBACK_SCENE_BACKGROUND_RING_FRAMES
        );
        if (ring_err == ESP_OK) {
            background_request = calloc(1, sizeof(*background_request));
            if (background_request) {
                background_request->ring = &background_ring;
                background_request->url = request->background_url;
                background_request->loop = request->background_loop;
                background_request->volume_percent = request->background_volume_percent;
                background_request->notify_task = xTaskGetCurrentTaskHandle();
                BaseType_t created = playback_create_task(
                    scene_background_task,
                    "scene_background",
                    PLAYBACK_SCENE_BACKGROUND_TASK_STACK,
                    background_request,
                    4,
                    &background_task,
                    0,
                    &background_request->task_with_caps
                );
                if (created != pdPASS) {
                    ESP_LOGW(TAG, "audio scene background task create failed");
                    free(background_request);
                    background_request = NULL;
                } else {
                    background_ready = scene_pcm_ring_wait_prebuffer(
                        &background_ring,
                        PLAYBACK_SCENE_PREBUFFER_TIMEOUT_MS
                    );
                    if (!background_ready) {
                        ESP_LOGW(TAG, "audio scene background unavailable; playing foreground only");
                    }
                }
            }
        } else {
            ESP_LOGW(
                TAG,
                "audio scene background buffer unavailable err=%s; playing foreground only",
                esp_err_to_name(ring_err)
            );
        }
    }

    if (s_abort) {
        err = ESP_FAIL;
        goto done;
    }

    scene_mixer_sink_t mixer = {
        .background = background_ready ? &background_ring : NULL,
        .ducking_target_percent = request->ducking_target_percent,
        .ducking_attack_ms = request->ducking_attack_ms,
        .ducking_release_ms = request->ducking_release_ms,
        .fade_out_ms = request->background_fade_out_ms,
    };
    playback_pcm_sink_t foreground_sink = {
        .ctx = &mixer,
        .begin = scene_mixer_sink_begin,
        .write = scene_mixer_sink_write,
        .end = scene_mixer_sink_end,
        .volume_percent = request->foreground_volume_percent,
    };
    err = stream_audio_url(request->foreground_url, &foreground_sink);

done:
    if (background_ring.lock) {
        scene_pcm_ring_stop(&background_ring);
    }
    if (background_task) {
        uint32_t waited_ms = 0;
        while (background_request && !background_request->done) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            waited_ms += 100;
            if (waited_ms == 3000 || (waited_ms > 3000 && (waited_ms % 5000) == 0)) {
                ESP_LOGW(TAG, "audio scene background still stopping after %u ms", (unsigned)waited_ms);
            }
        }
    }
    if (background_ring.lock) {
        ESP_LOGI(
            TAG,
            "audio scene background frames read=%u wrote=%u high_water=%u underrun=%u result=%s",
            (unsigned)background_ring.total_read_frames,
            (unsigned)background_ring.total_written_frames,
            (unsigned)background_ring.high_water_frames,
            (unsigned)background_ring.underrun_frames,
            background_request
                ? esp_err_to_name(background_request->result)
                : esp_err_to_name(ESP_ERR_NOT_FOUND)
        );
        scene_pcm_ring_destroy(&background_ring);
    }
    free(background_request);

    bool aborted = s_abort;
    char scene_id[TATER_PLAYBACK_SCENE_ID_MAX] = {0};
    if (request) {
        snprintf(scene_id, sizeof(scene_id), "%s", request->scene_id);
    }
    playback_mark_finished();
    if (!aborted && err == ESP_OK) {
        tater_protocol_send_audio_scene_finished(scene_id, true);
        tater_protocol_send_log("info", "audio scene finished");
    } else {
        tater_protocol_send_audio_scene_finished(scene_id, false);
        tater_protocol_send_log("warn", "audio scene stopped or failed");
    }

    if (request) {
        free(request->foreground_url);
        free(request->background_url);
    }
    free(request);
    playback_delete_current_task(task_with_caps);
}

static void media_session_wait_decoder(
    scene_background_args_t *request,
    scene_pcm_ring_t *ring,
    TaskHandle_t task
)
{
    if (ring && ring->lock) {
        scene_pcm_ring_stop(ring);
    }
    if (!task || !request) {
        return;
    }

    uint32_t waited_ms = 0;
    while (!request->done) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        waited_ms += 100;
        if (waited_ms == 3000 || (waited_ms > 3000 && (waited_ms % 5000) == 0)) {
            ESP_LOGW(TAG, "media decoder still stopping after %u ms", (unsigned)waited_ms);
        }
    }
}

static void media_session_free_decoder(scene_background_args_t **request_ptr)
{
    if (!request_ptr || !*request_ptr) {
        return;
    }
    scene_background_args_t *request = *request_ptr;
    free(request->url);
    free(request);
    *request_ptr = NULL;
}

static void media_session_reset_overlay(media_session_state_t *session)
{
    if (!session) {
        return;
    }

    scene_background_args_t *decoder = NULL;
    TaskHandle_t decoder_task = NULL;
    bool ring_initialized = false;
    char overlay_id[TATER_PLAYBACK_OVERLAY_ID_MAX] = {0};

    xSemaphoreTake(session->lock, portMAX_DELAY);
    decoder = session->overlay_decoder;
    decoder_task = session->overlay_decoder_task;
    ring_initialized = session->overlay_ring_initialized;
    snprintf(overlay_id, sizeof(overlay_id), "%s", session->overlay_id);
    session->overlay_pending = false;
    session->overlay_active = false;
    session->overlay_releasing = false;
    session->overlay_started_reported = false;
    session->overlay_start_at_us = 0;
    xSemaphoreGive(session->lock);

    media_session_wait_decoder(
        decoder,
        ring_initialized ? &session->overlay_ring : NULL,
        decoder_task
    );
    media_session_free_decoder(&decoder);
    if (ring_initialized) {
        scene_pcm_ring_destroy(&session->overlay_ring);
    }

    xSemaphoreTake(session->lock, portMAX_DELAY);
    session->overlay_decoder = NULL;
    session->overlay_decoder_task = NULL;
    session->overlay_ring_initialized = false;
    free(session->overlay_url);
    session->overlay_url = NULL;
    session->overlay_id[0] = '\0';
    xSemaphoreGive(session->lock);
}

static void media_session_finish_overlay(
    media_session_state_t *session,
    bool ok
)
{
    if (!session) {
        return;
    }
    char overlay_id[TATER_PLAYBACK_OVERLAY_ID_MAX] = {0};
    xSemaphoreTake(session->lock, portMAX_DELAY);
    snprintf(overlay_id, sizeof(overlay_id), "%s", session->overlay_id);
    xSemaphoreGive(session->lock);

    media_session_reset_overlay(session);
    tater_protocol_send_audio_overlay_finished(overlay_id, ok);
}

static bool media_session_overlay_flags(
    media_session_state_t *session,
    bool *pending,
    bool *active,
    bool *releasing
)
{
    if (!session || !session->lock) {
        return false;
    }
    xSemaphoreTake(session->lock, portMAX_DELAY);
    bool has_overlay =
        session->overlay_pending || session->overlay_active || session->overlay_releasing;
    if (pending) {
        *pending = session->overlay_pending;
    }
    if (active) {
        *active = session->overlay_active;
    }
    if (releasing) {
        *releasing = session->overlay_releasing;
    }
    xSemaphoreGive(session->lock);
    return has_overlay;
}

static const char *media_session_channel_name(tater_playback_channel_t channel)
{
    switch (channel) {
        case TATER_PLAYBACK_CHANNEL_LEFT:
            return "left";
        case TATER_PLAYBACK_CHANNEL_RIGHT:
            return "right";
        case TATER_PLAYBACK_CHANNEL_MONO:
            return "mono";
        case TATER_PLAYBACK_CHANNEL_STEREO:
        default:
            return "stereo";
    }
}

static bool media_session_wait_for_commit(
    media_session_state_t *session,
    int64_t *start_at_us
)
{
    if (!session || !session->lock || !start_at_us) {
        return false;
    }

    int64_t deadline_us =
        esp_timer_get_time() + ((int64_t)PLAYBACK_MEDIA_PREPARE_TIMEOUT_MS * 1000LL);
    while (!s_abort && esp_timer_get_time() < deadline_us) {
        xSemaphoreTake(session->lock, portMAX_DELAY);
        bool committed = session->committed;
        int64_t scheduled_start_us = session->scheduled_start_us;
        xSemaphoreGive(session->lock);
        if (committed) {
            *start_at_us = scheduled_start_us;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

static bool media_session_wait_until(int64_t start_at_us)
{
    while (!s_abort) {
        int64_t remaining_us = start_at_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return true;
        }
        if (remaining_us > 20000) {
            vTaskDelay(pdMS_TO_TICKS((remaining_us - 10000) / 1000));
        } else if (remaining_us > 2000) {
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            taskYIELD();
        }
    }
    return false;
}

static int32_t media_session_take_correction_step(media_session_state_t *session)
{
    if (!session || !session->lock) {
        return 0;
    }
    xSemaphoreTake(session->lock, portMAX_DELAY);
    int32_t correction = tater_playback_sync_slew_next_step(
        &session->correction_slew,
        PLAYBACK_MIX_CHUNK_FRAMES
    );
    xSemaphoreGive(session->lock);
    return correction;
}

static void media_session_restore_correction_step(
    media_session_state_t *session,
    int32_t correction
)
{
    if (!session || !session->lock || correction == 0) {
        return;
    }
    xSemaphoreTake(session->lock, portMAX_DELAY);
    tater_playback_sync_slew_restore_step(&session->correction_slew, correction);
    xSemaphoreGive(session->lock);
}

static int32_t media_session_take_jump_frames(media_session_state_t *session)
{
    if (!session || !session->lock) {
        return 0;
    }
    xSemaphoreTake(session->lock, portMAX_DELAY);
    int32_t frames = session->pending_jump_frames;
    session->pending_jump_frames = 0;
    xSemaphoreGive(session->lock);
    return frames;
}

static void media_session_restore_jump_frames(
    media_session_state_t *session,
    int32_t frames
)
{
    if (!session || !session->lock || frames <= 0) {
        return;
    }
    xSemaphoreTake(session->lock, portMAX_DELAY);
    int64_t pending = (int64_t)session->pending_jump_frames + frames;
    session->pending_jump_frames = pending > PLAYBACK_MEDIA_MAX_PENDING_CORRECTION_FRAMES
        ? PLAYBACK_MEDIA_MAX_PENDING_CORRECTION_FRAMES
        : (int32_t)pending;
    xSemaphoreGive(session->lock);
}

static void media_session_task(void *arg)
{
    media_session_state_t *session = (media_session_state_t *)arg;
    bool task_with_caps = session ? session->task_with_caps : false;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    bool speaker_started = false;
    bool ready_reported = false;
    bool started_reported = false;
    bool media_finished = false;
    bool rebuffering = false;
    uint64_t source_frames_written = 0;
    uint64_t output_frames_written = 0;
    uint64_t rejoin_frames_total = 0;
    uint64_t start_position_frames = 0;
    uint32_t render_clock_media_start_frame = 0;
    bool render_clock_available = false;
    int32_t correction_since_report = 0;
    uint32_t underrun_events = 0;
    uint32_t rejoin_count = 0;
    uint32_t recovery_fade_frames_remaining = 0;
    int64_t scheduled_start_us = 0;
    int64_t actual_start_us = 0;
    int64_t last_playhead_us = 0;
    tater_playback_mix_state_t mix;
    char session_id[TATER_PLAYBACK_MEDIA_SESSION_ID_MAX] = {0};
    char group_id[TATER_PLAYBACK_MEDIA_GROUP_ID_MAX] = {0};
    char prepare_reply_to[TATER_PLAYBACK_REQUEST_ID_MAX] = {0};
    tater_playback_channel_t media_channel = TATER_PLAYBACK_CHANNEL_STEREO;
    bool prepare_requested = false;
    bool complete_visual_state = false;
    bool tool_visual_state = false;
    uint32_t start_position_ms = 0;

    if (!session || !session->lock || !session->media_url || !session->media_url[0]) {
        goto done;
    }
    xSemaphoreTake(session->lock, portMAX_DELAY);
    snprintf(session_id, sizeof(session_id), "%s", session->session_id);
    snprintf(group_id, sizeof(group_id), "%s", session->group_id);
    snprintf(prepare_reply_to, sizeof(prepare_reply_to), "%s", session->prepare_reply_to);
    media_channel = session->media_channel;
    prepare_requested = session->prepare_requested;
    complete_visual_state = session->complete_visual_state;
    tool_visual_state = session->tool_visual_state;
    start_position_ms = session->media_start_position_ms;
    xSemaphoreGive(session->lock);
    s_playing = true;
    playback_log_heap("media session start");

    err = scene_pcm_ring_init(&session->media_ring, PLAYBACK_MEDIA_RING_FRAMES);
    if (err != ESP_OK) {
        goto done;
    }
    session->media_ring_initialized = true;

    session->media_decoder = calloc(1, sizeof(*session->media_decoder));
    if (!session->media_decoder) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    session->media_decoder->ring = &session->media_ring;
    session->media_decoder->url = strdup(session->media_url);
    session->media_decoder->loop = session->media_loop;
    session->media_decoder->volume_percent = session->media_volume_percent;
    session->media_decoder->live_volume_percent = &session->media_volume_percent;
    // Music sessions own an absolute 0-100 scale. Voice/tone playback still
    // follows the satellite master volume, while a media session at 100 can
    // reach the hardware maximum just like its player UI indicates.
    session->media_decoder->absolute_volume = true;
    session->media_decoder->skip_frames_remaining =
        ((uint64_t)start_position_ms * (uint64_t)TATER_SPK_SAMPLE_RATE) / 1000ULL;
    session->media_decoder->notify_task = xTaskGetCurrentTaskHandle();
    if (!session->media_decoder->url) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    BaseType_t decoder_created = playback_create_task(
        scene_background_task,
        "media_decoder",
        PLAYBACK_SCENE_BACKGROUND_TASK_STACK,
        session->media_decoder,
        4,
        &session->media_decoder_task,
        0,
        &session->media_decoder->task_with_caps
    );
    if (decoder_created != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    uint32_t media_prebuffer_timeout_ms = PLAYBACK_SCENE_PREBUFFER_TIMEOUT_MS;
    if (start_position_ms > 0) {
        uint64_t seek_timeout_ms =
            (uint64_t)PLAYBACK_SCENE_PREBUFFER_TIMEOUT_MS
            + ((uint64_t)start_position_ms / 15ULL);
        media_prebuffer_timeout_ms = seek_timeout_ms > 60000ULL
            ? 60000U
            : (uint32_t)seek_timeout_ms;
    }
    if (!scene_pcm_ring_wait_prebuffer(
            &session->media_ring,
            media_prebuffer_timeout_ms
        )) {
        err = session->media_decoder->done
            ? session->media_decoder->result
            : ESP_ERR_TIMEOUT;
        goto done;
    }

    err = playback_speaker_begin();
    if (err != ESP_OK) {
        goto done;
    }
    speaker_started = true;

    size_t ready_buffered_frames = 0;
    bool ready_ended = false;
    scene_pcm_ring_snapshot(&session->media_ring, &ready_buffered_frames, &ready_ended);
    (void)ready_ended;
    if (prepare_requested) {
        xSemaphoreTake(session->lock, portMAX_DELAY);
        session->prepared = true;
        xSemaphoreGive(session->lock);
        tater_protocol_send_media_session_ready(
            session_id,
            group_id,
            prepare_reply_to,
            true,
            (uint32_t)ready_buffered_frames
        );
        ready_reported = true;
        if (!media_session_wait_for_commit(session, &scheduled_start_us)) {
            err = ESP_ERR_TIMEOUT;
            goto done;
        }
    } else {
        scheduled_start_us = esp_timer_get_time();
    }

    if (scheduled_start_us <= 0) {
        scheduled_start_us = esp_timer_get_time();
    }
    if (!media_session_wait_until(scheduled_start_us)) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    tater_audio_render_clock_t render_clock_start = {0};
    render_clock_available = tater_audio_speaker_render_clock_snapshot(
        &render_clock_start
    );
    render_clock_media_start_frame = render_clock_start.submitted_frames;
    tater_playback_mix_init(&mix, 100);
    if (complete_visual_state) {
        tater_protocol_start_media_session_visual(tool_visual_state);
    }
    start_position_frames =
        ((uint64_t)start_position_ms * (uint64_t)TATER_SPK_SAMPLE_RATE) / 1000ULL;
    source_frames_written = start_position_frames;

    int16_t media_input_frames[(PLAYBACK_MIX_CHUNK_FRAMES + 1) * TATER_SPK_CHANNELS];
    int16_t media_frames[PLAYBACK_MIX_CHUNK_FRAMES * TATER_SPK_CHANNELS];
    int16_t foreground_frames[PLAYBACK_MIX_CHUNK_FRAMES * TATER_SPK_CHANNELS];
    int16_t mixed_frames[PLAYBACK_MIX_CHUNK_FRAMES * TATER_SPK_CHANNELS];

    while (!s_abort) {
        size_t media_fill = 0;
        bool media_ended = false;
        scene_pcm_ring_snapshot(&session->media_ring, &media_fill, &media_ended);
        media_finished = media_ended && media_fill == 0;

        bool overlay_pending = false;
        bool overlay_active = false;
        bool overlay_releasing = false;
        bool has_overlay = media_session_overlay_flags(
            session,
            &overlay_pending,
            &overlay_active,
            &overlay_releasing
        );

        if (playback_recovery_watch_rebuffer(
                rebuffering,
                !has_overlay,
                session_id
            )) {
            err = ESP_ERR_TIMEOUT;
            break;
        }

        if (overlay_pending) {
            size_t overlay_fill = 0;
            bool overlay_ended = false;
            scene_pcm_ring_snapshot(&session->overlay_ring, &overlay_fill, &overlay_ended);
            size_t prebuffer_frames =
                ((size_t)TATER_SPK_SAMPLE_RATE * PLAYBACK_SCENE_PREBUFFER_MS) / 1000;
            xSemaphoreTake(session->lock, portMAX_DELAY);
            int64_t overlay_start_at_us = session->overlay_start_at_us;
            xSemaphoreGive(session->lock);
            bool overlay_start_due =
                overlay_start_at_us <= 0 || esp_timer_get_time() >= overlay_start_at_us;
            if (
                overlay_start_due
                && (overlay_fill >= prebuffer_frames || (overlay_ended && overlay_fill > 0))
            ) {
                xSemaphoreTake(session->lock, portMAX_DELAY);
                session->overlay_pending = false;
                session->overlay_active = true;
                overlay_pending = false;
                overlay_active = true;
                uint32_t attack_frames =
                    ((uint32_t)TATER_SPK_SAMPLE_RATE * session->ducking_attack_ms) / 1000;
                tater_playback_mix_set_background(
                    &mix,
                    session->ducking_target_percent,
                    attack_frames
                );
                if (!session->overlay_started_reported) {
                    tater_protocol_send_audio_overlay_started(session->overlay_id);
                    session->overlay_started_reported = true;
                }
                xSemaphoreGive(session->lock);
            } else if (overlay_ended && overlay_fill == 0) {
                media_session_finish_overlay(session, false);
                has_overlay = false;
                overlay_pending = false;
            }
        }

        memset(media_input_frames, 0, sizeof(media_input_frames));
        memset(media_frames, 0, sizeof(media_frames));
        memset(foreground_frames, 0, sizeof(foreground_frames));
        bool recovery_silence = false;
        if (rebuffering && !has_overlay) {
            size_t recovery_fill = 0;
            bool recovery_ended = false;
            scene_pcm_ring_snapshot(
                &session->media_ring,
                &recovery_fill,
                &recovery_ended
            );
            size_t recovery_prebuffer_frames =
                ((size_t)TATER_SPK_SAMPLE_RATE * PLAYBACK_MEDIA_REBUFFER_MS) / 1000;
            int64_t recovery_now_us = esp_timer_get_time();
            uint64_t desired_source_frames = start_position_frames;
            if (recovery_now_us > scheduled_start_us) {
                desired_source_frames +=
                    ((uint64_t)(recovery_now_us - scheduled_start_us)
                        * (uint64_t)TATER_SPK_SAMPLE_RATE) / 1000000ULL;
            }
            if (recovery_fill >= recovery_prebuffer_frames || recovery_ended) {
                uint64_t phase_gap = desired_source_frames > source_frames_written
                    ? desired_source_frames - source_frames_written
                    : 0;
                size_t retain_frames = recovery_ended ? 0 : recovery_prebuffer_frames;
                size_t available_to_discard = recovery_fill > retain_frames
                    ? recovery_fill - retain_frames
                    : 0;
                size_t discard_target = phase_gap < (uint64_t)available_to_discard
                    ? (size_t)phase_gap
                    : available_to_discard;
                size_t discarded = scene_pcm_ring_discard(
                    &session->media_ring,
                    discard_target
                );
                source_frames_written += discarded;
                rejoin_frames_total += discarded;
                if (
                    source_frames_written + PLAYBACK_MEDIA_REJOIN_TOLERANCE_FRAMES
                        >= desired_source_frames
                ) {
                    rebuffering = false;
                    rejoin_count++;
                    recovery_fade_frames_remaining =
                        ((uint32_t)TATER_SPK_SAMPLE_RATE
                            * PLAYBACK_MEDIA_REJOIN_FADE_MS) / 1000;
                    ESP_LOGI(
                        TAG,
                        "media session timeline rejoined id=%s skipped=%" PRIu64 " rejoins=%u",
                        session_id[0] ? session_id : "-",
                        rejoin_frames_total,
                        (unsigned)rejoin_count
                    );
                }
            }
            recovery_silence = rebuffering;
        }

        size_t media_output_frames = 0;
        size_t got_media = 0;
        if (recovery_silence) {
            media_output_frames = PLAYBACK_MIX_CHUNK_FRAMES;
        } else {
            int32_t jump_frames = !has_overlay
                ? media_session_take_jump_frames(session)
                : 0;
            if (jump_frames > 0) {
                size_t discarded = scene_pcm_ring_discard(
                    &session->media_ring,
                    (size_t)jump_frames
                );
                source_frames_written += discarded;
                correction_since_report += (int32_t)discarded;
                if (discarded < (size_t)jump_frames) {
                    media_session_restore_jump_frames(
                        session,
                        jump_frames - (int32_t)discarded
                    );
                }
            }
            int32_t correction = !has_overlay
                ? media_session_take_correction_step(session)
                : 0;
            size_t requested_media_frames = (size_t)(
                (int32_t)PLAYBACK_MIX_CHUNK_FRAMES + correction
            );
            got_media = scene_pcm_ring_read(
                &session->media_ring,
                media_input_frames,
                requested_media_frames,
                media_finished ? 0 : pdMS_TO_TICKS(30)
            );
            source_frames_written += got_media;

            size_t current_fill = 0;
            bool current_ended = false;
            scene_pcm_ring_snapshot(&session->media_ring, &current_fill, &current_ended);
            (void)current_fill;
            if (got_media == requested_media_frames && got_media > 0) {
                tater_playback_sync_resample_stereo(
                    media_input_frames,
                    got_media,
                    media_frames,
                    PLAYBACK_MIX_CHUNK_FRAMES
                );
                media_output_frames = PLAYBACK_MIX_CHUNK_FRAMES;
                correction_since_report += correction;
            } else {
                media_session_restore_correction_step(session, correction);
                media_output_frames = got_media < PLAYBACK_MIX_CHUNK_FRAMES
                    ? got_media
                    : PLAYBACK_MIX_CHUNK_FRAMES;
                if (media_output_frames > 0) {
                    memcpy(
                        media_frames,
                        media_input_frames,
                        media_output_frames * TATER_SPK_CHANNELS * sizeof(int16_t)
                    );
                }
                if (!current_ended && !has_overlay) {
                    rebuffering = true;
                    underrun_events++;
                    media_output_frames = PLAYBACK_MIX_CHUNK_FRAMES;
                    ESP_LOGW(
                        TAG,
                        "media session underrun id=%s got=%u wanted=%u events=%u",
                        session_id[0] ? session_id : "-",
                        (unsigned)got_media,
                        (unsigned)requested_media_frames,
                        (unsigned)underrun_events
                    );
                }
            }
            if (media_output_frames > 0) {
                tater_playback_route_channel(
                    media_channel,
                    media_frames,
                    media_output_frames
                );
                tater_playback_sync_fade_in(
                    media_frames,
                    media_output_frames,
                    &recovery_fade_frames_remaining,
                    ((uint32_t)TATER_SPK_SAMPLE_RATE
                        * PLAYBACK_MEDIA_REJOIN_FADE_MS) / 1000
                );
            }
        }
        size_t got_foreground = 0;
        bool release_after_write = false;

        if (overlay_active) {
            got_foreground = scene_pcm_ring_read(
                &session->overlay_ring,
                foreground_frames,
                PLAYBACK_MIX_CHUNK_FRAMES,
                pdMS_TO_TICKS(30)
            );
            size_t overlay_fill = 0;
            bool overlay_ended = false;
            scene_pcm_ring_snapshot(&session->overlay_ring, &overlay_fill, &overlay_ended);
            release_after_write = overlay_ended && overlay_fill == 0;
        }

        size_t output_frames = media_output_frames > got_foreground
            ? media_output_frames
            : got_foreground;
        if (output_frames > 0) {
            tater_playback_mix_frames(
                &mix,
                overlay_active ? foreground_frames : NULL,
                media_frames,
                mixed_frames,
                output_frames
            );
            int64_t first_submit_us = started_reported ? 0 : esp_timer_get_time();
            err = playback_speaker_write(mixed_frames, output_frames);
            if (err != ESP_OK) {
                break;
            }
            output_frames_written += output_frames;
            if (!started_reported) {
                actual_start_us = first_submit_us;
                tater_protocol_send_media_session_started(
                    session_id,
                    group_id,
                    media_session_channel_name(media_channel),
                    scheduled_start_us,
                    actual_start_us
                );
                started_reported = true;
                last_playhead_us = actual_start_us;
                ESP_LOGI(
                    TAG,
                    "media session started id=%s group=%s channel=%s scheduled=%" PRId64 " actual=%" PRId64,
                    session_id[0] ? session_id : "-",
                    group_id[0] ? group_id : "-",
                    media_session_channel_name(media_channel),
                    scheduled_start_us,
                    actual_start_us
                );
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (
            now_us - last_playhead_us
                >= ((int64_t)PLAYBACK_MEDIA_PLAYHEAD_INTERVAL_MS * 1000LL)
        ) {
            size_t buffered_frames = 0;
            bool media_ring_ended = false;
            scene_pcm_ring_snapshot(
                &session->media_ring,
                &buffered_frames,
                &media_ring_ended
            );
            (void)media_ring_ended;
            xSemaphoreTake(session->lock, portMAX_DELAY);
            session->source_frames = source_frames_written;
            session->output_frames = output_frames_written;
            xSemaphoreGive(session->lock);
            uint64_t rendered_frames = tater_playback_sync_rendered_source_frames(
                source_frames_written,
                start_position_frames,
                TATER_MEDIA_RENDER_LATENCY_FRAMES
            );
            uint32_t output_latency_frames = TATER_MEDIA_RENDER_LATENCY_FRAMES;
            tater_audio_render_clock_t render_clock = {0};
            if (
                render_clock_available
                && output_frames_written > 0
                && tater_audio_speaker_render_clock_snapshot(&render_clock)
            ) {
                uint64_t rendered_output_frames = 0;
                if (render_clock.completed_frames > render_clock_media_start_frame) {
                    rendered_output_frames = (uint32_t)(
                        render_clock.completed_frames - render_clock_media_start_frame
                    );
                }
                if (rendered_output_frames > output_frames_written) {
                    rendered_output_frames = output_frames_written;
                }
                uint64_t source_relative_frames = source_frames_written > start_position_frames
                    ? source_frames_written - start_position_frames
                    : 0;
                uint64_t rendered_source_relative_frames = (
                    (rendered_output_frames * source_relative_frames)
                    + (output_frames_written / 2ULL)
                ) / output_frames_written;
                rendered_frames = start_position_frames + rendered_source_relative_frames;
                uint64_t pending_output_frames = output_frames_written - rendered_output_frames;
                output_latency_frames = pending_output_frames > UINT32_MAX
                    ? UINT32_MAX
                    : (uint32_t)pending_output_frames;
            }
            tater_protocol_send_media_session_playhead(
                session_id,
                group_id,
                media_session_channel_name(media_channel),
                source_frames_written,
                rendered_frames,
                output_frames_written,
                output_latency_frames,
                (uint32_t)buffered_frames,
                now_us,
                scheduled_start_us,
                correction_since_report,
                rebuffering,
                underrun_events,
                rejoin_count,
                rejoin_frames_total
            );
            correction_since_report = 0;
            last_playhead_us = now_us;
        }

        if (release_after_write) {
            xSemaphoreTake(session->lock, portMAX_DELAY);
            session->overlay_active = false;
            session->overlay_releasing = true;
            overlay_active = false;
            overlay_releasing = true;
            uint32_t release_frames =
                ((uint32_t)TATER_SPK_SAMPLE_RATE * session->ducking_release_ms) / 1000;
            tater_playback_mix_set_background(&mix, 100, release_frames);
            xSemaphoreGive(session->lock);
        }

        if (
            overlay_releasing
            && (mix.ramp_frames_remaining == 0 || (media_finished && got_media == 0))
        ) {
            media_session_finish_overlay(session, true);
            has_overlay = false;
            overlay_releasing = false;
        }

        if (media_finished && !has_overlay) {
            err = session->media_decoder && session->media_decoder->result != ESP_OK
                ? session->media_decoder->result
                : ESP_OK;
            break;
        }
        if (output_frames == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

done:
    if (prepare_requested && !ready_reported) {
        tater_protocol_send_media_session_ready(
            session_id,
            group_id,
            prepare_reply_to,
            false,
            0
        );
    }
    if (session && session->lock) {
        xSemaphoreTake(session->lock, portMAX_DELAY);
        session->accepting_overlays = false;
        bool overlay_in_progress =
            session->overlay_pending || session->overlay_active || session->overlay_releasing;
        xSemaphoreGive(session->lock);
        if (overlay_in_progress) {
            media_session_finish_overlay(session, false);
        }
    }

    if (session && session->media_ring_initialized) {
        media_session_wait_decoder(
            session->media_decoder,
            &session->media_ring,
            session->media_decoder_task
        );
    }
    if (session) {
        media_session_free_decoder(&session->media_decoder);
        session->media_decoder_task = NULL;
        if (session->media_ring_initialized) {
            scene_pcm_ring_destroy(&session->media_ring);
            session->media_ring_initialized = false;
        }
    }
    if (speaker_started) {
        esp_err_t end_err = playback_speaker_end();
        if (err == ESP_OK && end_err != ESP_OK) {
            err = end_err;
        }
    }

    bool aborted = s_abort;
    if (session && session->lock) {
        xSemaphoreTake(session->lock, portMAX_DELAY);
        free(session->media_url);
        session->media_url = NULL;
        session->active = false;
        session->accepting_overlays = false;
        session->session_id[0] = '\0';
        session->group_id[0] = '\0';
        session->prepare_reply_to[0] = '\0';
        session->prepared = false;
        session->committed = false;
        session->complete_visual_state = false;
        session->tool_visual_state = false;
        session->scheduled_start_us = 0;
        session->media_start_position_ms = 0;
        tater_playback_sync_slew_init(&session->correction_slew);
        session->pending_jump_frames = 0;
        session->source_frames = 0;
        session->output_frames = 0;
        xSemaphoreGive(session->lock);
    }
    playback_mark_finished();

    bool ok = !aborted && started_reported && err == ESP_OK;
    tater_protocol_send_media_session_finished(
        session_id,
        ok,
        complete_visual_state
    );
    ESP_LOGI(
        TAG,
        "media session finished id=%s ok=%d",
        session_id[0] ? session_id : "-",
        ok
    );
    playback_delete_current_task(task_with_caps);
}

static void playback_memory_task(void *arg)
{
    playback_memory_args_t *request = (playback_memory_args_t *)arg;
    bool task_with_caps = request ? request->task_with_caps : false;
    wav_info_t wav;
    esp_err_t err = ESP_OK;

    s_playing = true;
    ESP_LOGI(TAG, "local wav playback label=%s bytes=%u", request ? request->label : "", (unsigned)(request ? request->len : 0));

    if (!request || !request->data || request->len == 0) {
        err = ESP_ERR_INVALID_ARG;
    } else if (!parse_wav(request->data, request->len, &wav)) {
        ESP_LOGE(TAG, "unsupported local wav");
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "local wav rate=%" PRIu32 " channels=%u bits=%u frames=%u", wav.sample_rate, wav.channels, wav.bits_per_sample, (unsigned)wav.frame_count);
        err = play_wav(&wav);
    }

    bool playback_ok = !s_abort && err == ESP_OK;
    if (request && request->free_data && request->data) {
        free((void *)request->data);
    }
    free(request);
    playback_mark_finished();
    if (playback_ok) {
        tater_protocol_send_log("info", "local wake sound finished");
    } else {
        tater_protocol_send_log("warn", "local wake sound stopped or failed");
    }
    playback_delete_current_task(task_with_caps);
}

static void tone_task(void *arg)
{
    tone_args_t *tone = (tone_args_t *)arg;
    bool task_with_caps = tone ? tone->task_with_caps : false;
    s_playing = true;
    ESP_LOGI(
        TAG,
        "playback tone frequency=%" PRIu32 " duration_ms=%" PRIu32 " volume_percent=%u",
        tone->frequency_hz,
        tone->duration_ms,
        tone->volume_percent
    );
    tater_protocol_send_log("info", "tone started");
    esp_err_t err = play_tone(tone->frequency_hz, tone->duration_ms, tone->volume_percent);
    bool aborted = s_abort;
    playback_mark_finished();
    if (tone->notify_finished) {
        if (!aborted && err == ESP_OK) {
            tater_protocol_send_playback_finished();
            tater_protocol_send_log("info", "tone finished");
        } else {
            tater_protocol_send_playback_finished_status(false, false);
            tater_protocol_send_log("warn", "tone stopped or failed");
        }
    } else if (!aborted && err == ESP_OK) {
        tater_protocol_send_log("info", "local tone finished");
    } else {
        tater_protocol_send_log("warn", "local tone stopped or failed");
    }
    free(tone);
    playback_delete_current_task(task_with_caps);
}

esp_err_t tater_playback_init(void)
{
    s_abort = false;
    s_playing = false;
    s_task = NULL;
    if (!s_lifecycle_lock) {
        s_lifecycle_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lifecycle_lock, ESP_ERR_NO_MEM, TAG, "playback lifecycle mutex failed");
    }
    if (!s_media_session.lock) {
        s_media_session.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_media_session.lock, ESP_ERR_NO_MEM, TAG, "media session mutex failed");
    }
    playback_recovery_begin_session();
    if (!s_recovery_task) {
        /*
         * The playback watchdog never programs flash, so its persistent stack
         * may live in PSRAM. Keeping it out of internal RAM leaves a contiguous
         * block available for the cache-sensitive OTA task when an update is
         * requested after the rest of the audio runtime has initialized.
         */
        BaseType_t recovery_created = playback_create_task(
            playback_recovery_task,
            "playback_guard",
            PLAYBACK_RECOVERY_TASK_STACK,
            NULL,
            4,
            &s_recovery_task,
            0,
            NULL
        );
        ESP_RETURN_ON_FALSE(
            recovery_created == pdPASS,
            ESP_ERR_NO_MEM,
            TAG,
            "playback watchdog task failed"
        );
    }
    esp_err_t codec_err = audio_codec_register_once();
    if (codec_err != ESP_OK) {
        ESP_LOGW(TAG, "mp3/flac decoder registration deferred err=%s", esp_err_to_name(codec_err));
    }
    return ESP_OK;
}

static esp_err_t play_url(const char *url, bool notify_finished)
{
    if (!url || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!playback_begin_start()) {
        return ESP_ERR_TIMEOUT;
    }
    playback_log_heap("playback start");

    playback_args_t *request = calloc(1, sizeof(*request));
    if (!request) {
        playback_log_heap("playback request alloc failed");
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    request->url = strdup(url);
    if (!request->url) {
        free(request);
        playback_log_heap("playback url alloc failed");
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    request->notify_finished = notify_finished;
    BaseType_t ok = playback_create_task(
        playback_task,
        "tater_playback",
        PLAYBACK_URL_TASK_STACK,
        request,
        5,
        &s_task,
        1,
        &request->task_with_caps
    );
    if (ok != pdPASS) {
        free(request->url);
        free(request);
        playback_log_heap("playback task create failed");
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    playback_end_start();
    return ESP_OK;
}

esp_err_t tater_playback_play_url(const char *url)
{
    return play_url(url, true);
}

esp_err_t tater_playback_play_url_local(const char *url)
{
    return play_url(url, false);
}

esp_err_t tater_playback_play_scene(const tater_playback_scene_t *scene)
{
    if (!scene || !scene->foreground_url || !scene->foreground_url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!playback_begin_start()) {
        return ESP_ERR_TIMEOUT;
    }
    playback_log_heap("audio scene start");

    scene_args_t *request = calloc(1, sizeof(*request));
    if (!request) {
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    request->foreground_url = strdup(scene->foreground_url);
    if (!request->foreground_url) {
        free(request);
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    if (scene->background_url && scene->background_url[0]) {
        request->background_url = strdup(scene->background_url);
        if (!request->background_url) {
            free(request->foreground_url);
            free(request);
            return playback_start_failed(ESP_ERR_NO_MEM);
        }
    }

    snprintf(
        request->scene_id,
        sizeof(request->scene_id),
        "%s",
        scene->scene_id ? scene->scene_id : ""
    );
    request->foreground_volume_percent = scene->foreground_volume_percent > 100
        ? 100
        : scene->foreground_volume_percent;
    request->background_volume_percent = scene->background_volume_percent > 100
        ? 100
        : scene->background_volume_percent;
    request->ducking_target_percent = scene->ducking_target_percent > 100
        ? 100
        : scene->ducking_target_percent;
    request->ducking_attack_ms = scene->ducking_attack_ms;
    request->ducking_release_ms = scene->ducking_release_ms;
    request->background_fade_out_ms = scene->background_fade_out_ms;
    request->background_loop = scene->background_loop;

    BaseType_t ok = playback_create_task(
        scene_task,
        "tater_scene",
        PLAYBACK_URL_TASK_STACK,
        request,
        5,
        &s_task,
        1,
        &request->task_with_caps
    );
    if (ok != pdPASS) {
        free(request->foreground_url);
        free(request->background_url);
        free(request);
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    playback_end_start();
    return ESP_OK;
}

esp_err_t tater_playback_start_media_session(const tater_playback_media_session_t *media)
{
    if (!media || !media->url || !media->url[0] || !s_media_session.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!playback_begin_start()) {
        return ESP_ERR_TIMEOUT;
    }
    playback_log_heap("media session request");

    xSemaphoreTake(s_media_session.lock, portMAX_DELAY);
    free(s_media_session.media_url);
    s_media_session.media_url = strdup(media->url);
    if (!s_media_session.media_url) {
        xSemaphoreGive(s_media_session.lock);
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    snprintf(
        s_media_session.session_id,
        sizeof(s_media_session.session_id),
        "%s",
        media->session_id ? media->session_id : ""
    );
    snprintf(
        s_media_session.group_id,
        sizeof(s_media_session.group_id),
        "%s",
        media->group_id ? media->group_id : ""
    );
    snprintf(
        s_media_session.prepare_reply_to,
        sizeof(s_media_session.prepare_reply_to),
        "%s",
        media->prepare_reply_to ? media->prepare_reply_to : ""
    );
    s_media_session.media_volume_percent =
        media->volume_percent > 100 ? 100 : media->volume_percent;
    s_media_session.media_start_position_ms = media->start_position_ms;
    s_media_session.media_channel =
        media->channel <= TATER_PLAYBACK_CHANNEL_MONO
        ? media->channel
        : TATER_PLAYBACK_CHANNEL_STEREO;
    s_media_session.media_loop = media->loop;
    s_media_session.complete_visual_state = media->complete_visual_state;
    s_media_session.tool_visual_state = media->tool_visual_state;
    s_media_session.prepare_requested = media->prepare;
    s_media_session.prepared = false;
    s_media_session.committed = !media->prepare;
    s_media_session.scheduled_start_us = 0;
    tater_playback_sync_slew_init(&s_media_session.correction_slew);
    s_media_session.pending_jump_frames = 0;
    s_media_session.source_frames = 0;
    s_media_session.output_frames = 0;
    s_media_session.media_ring_initialized = false;
    s_media_session.media_decoder = NULL;
    s_media_session.media_decoder_task = NULL;
    s_media_session.overlay_ring_initialized = false;
    s_media_session.overlay_decoder = NULL;
    s_media_session.overlay_decoder_task = NULL;
    s_media_session.overlay_pending = false;
    s_media_session.overlay_active = false;
    s_media_session.overlay_releasing = false;
    s_media_session.overlay_started_reported = false;
    s_media_session.overlay_start_at_us = 0;
    s_media_session.active = true;
    s_media_session.accepting_overlays = true;
    xSemaphoreGive(s_media_session.lock);

    BaseType_t ok = playback_create_task(
        media_session_task,
        "tater_media",
        PLAYBACK_URL_TASK_STACK,
        &s_media_session,
        TATER_MEDIA_PLAYBACK_TASK_PRIORITY,
        &s_task,
        1,
        &s_media_session.task_with_caps
    );
    if (ok != pdPASS) {
        xSemaphoreTake(s_media_session.lock, portMAX_DELAY);
        free(s_media_session.media_url);
        s_media_session.media_url = NULL;
        s_media_session.active = false;
        s_media_session.accepting_overlays = false;
        xSemaphoreGive(s_media_session.lock);
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    playback_end_start();
    return ESP_OK;
}

esp_err_t tater_playback_commit_media_session(const char *session_id, int64_t start_at_us)
{
    if (!session_id || !session_id[0] || !s_media_session.lock || start_at_us <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_media_session.lock, portMAX_DELAY);
    if (
        !s_media_session.active
        || !s_media_session.prepare_requested
        || strcmp(s_media_session.session_id, session_id) != 0
    ) {
        xSemaphoreGive(s_media_session.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_media_session.scheduled_start_us = start_at_us;
    s_media_session.committed = true;
    xSemaphoreGive(s_media_session.lock);
    return ESP_OK;
}

esp_err_t tater_playback_adjust_media_session(
    const char *session_id,
    int32_t correction_frames,
    const char *mode,
    uint32_t settle_ms
)
{
    if (!session_id || !session_id[0] || !s_media_session.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    if (correction_frames > PLAYBACK_MEDIA_MAX_PENDING_CORRECTION_FRAMES) {
        correction_frames = PLAYBACK_MEDIA_MAX_PENDING_CORRECTION_FRAMES;
    } else if (correction_frames < -PLAYBACK_MEDIA_MAX_PENDING_CORRECTION_FRAMES) {
        correction_frames = -PLAYBACK_MEDIA_MAX_PENDING_CORRECTION_FRAMES;
    }

    xSemaphoreTake(s_media_session.lock, portMAX_DELAY);
    if (
        !s_media_session.active
        || strcmp(s_media_session.session_id, session_id) != 0
    ) {
        xSemaphoreGive(s_media_session.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (mode && strcmp(mode, "jump") == 0 && correction_frames > 0) {
        int64_t pending =
            (int64_t)s_media_session.pending_jump_frames + correction_frames;
        s_media_session.pending_jump_frames =
            pending > PLAYBACK_MEDIA_MAX_PENDING_CORRECTION_FRAMES
            ? PLAYBACK_MEDIA_MAX_PENDING_CORRECTION_FRAMES
            : (int32_t)pending;
    } else {
        uint32_t normalized_settle_ms = settle_ms > 0 ? settle_ms : 1000;
        if (normalized_settle_ms > 10000) {
            normalized_settle_ms = 10000;
        }
        uint64_t settle_frames_64 =
            ((uint64_t)TATER_SPK_SAMPLE_RATE * normalized_settle_ms) / 1000ULL;
        uint32_t settle_frames = settle_frames_64 > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)settle_frames_64;
        tater_playback_sync_slew_replace(
            &s_media_session.correction_slew,
            correction_frames,
            settle_frames,
            PLAYBACK_MEDIA_MAX_PENDING_CORRECTION_FRAMES
        );
    }
    xSemaphoreGive(s_media_session.lock);
    return ESP_OK;
}

esp_err_t tater_playback_set_media_session_volume(
    const char *session_id,
    uint8_t volume_percent
)
{
    if (!s_media_session.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume_percent > 100) {
        volume_percent = 100;
    }

    xSemaphoreTake(s_media_session.lock, portMAX_DELAY);
    if (
        !s_media_session.active
        || (session_id && session_id[0]
            && strcmp(s_media_session.session_id, session_id) != 0)
    ) {
        xSemaphoreGive(s_media_session.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_media_session.media_volume_percent = volume_percent;
    xSemaphoreGive(s_media_session.lock);
    return ESP_OK;
}

esp_err_t tater_playback_play_overlay(const tater_playback_overlay_t *overlay)
{
    if (!overlay || !overlay->foreground_url || !overlay->foreground_url[0] || !s_media_session.lock) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_media_session.lock, portMAX_DELAY);
    if (
        !s_media_session.active
        || !s_media_session.accepting_overlays
        || s_abort
        || s_media_session.overlay_pending
        || s_media_session.overlay_active
        || s_media_session.overlay_releasing
        || s_media_session.overlay_ring_initialized
    ) {
        xSemaphoreGive(s_media_session.lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ring_err = scene_pcm_ring_init(
        &s_media_session.overlay_ring,
        PLAYBACK_OVERLAY_RING_FRAMES
    );
    if (ring_err != ESP_OK) {
        xSemaphoreGive(s_media_session.lock);
        return ring_err;
    }
    s_media_session.overlay_ring_initialized = true;
    s_media_session.overlay_url = strdup(overlay->foreground_url);
    s_media_session.overlay_decoder = calloc(1, sizeof(*s_media_session.overlay_decoder));
    if (!s_media_session.overlay_url || !s_media_session.overlay_decoder) {
        free(s_media_session.overlay_url);
        s_media_session.overlay_url = NULL;
        free(s_media_session.overlay_decoder);
        s_media_session.overlay_decoder = NULL;
        scene_pcm_ring_destroy(&s_media_session.overlay_ring);
        s_media_session.overlay_ring_initialized = false;
        xSemaphoreGive(s_media_session.lock);
        return ESP_ERR_NO_MEM;
    }

    snprintf(
        s_media_session.overlay_id,
        sizeof(s_media_session.overlay_id),
        "%s",
        overlay->overlay_id ? overlay->overlay_id : ""
    );
    s_media_session.overlay_volume_percent =
        overlay->foreground_volume_percent > 100 ? 100 : overlay->foreground_volume_percent;
    s_media_session.ducking_target_percent =
        overlay->ducking_target_percent > 100 ? 100 : overlay->ducking_target_percent;
    s_media_session.ducking_attack_ms = overlay->ducking_attack_ms;
    s_media_session.ducking_release_ms = overlay->ducking_release_ms;
    s_media_session.overlay_start_at_us = overlay->start_at_us;
    s_media_session.overlay_decoder->ring = &s_media_session.overlay_ring;
    s_media_session.overlay_decoder->url = strdup(s_media_session.overlay_url);
    s_media_session.overlay_decoder->loop = false;
    s_media_session.overlay_decoder->volume_percent = s_media_session.overlay_volume_percent;
    s_media_session.overlay_decoder->notify_task = s_task;
    if (!s_media_session.overlay_decoder->url) {
        free(s_media_session.overlay_url);
        s_media_session.overlay_url = NULL;
        free(s_media_session.overlay_decoder);
        s_media_session.overlay_decoder = NULL;
        scene_pcm_ring_destroy(&s_media_session.overlay_ring);
        s_media_session.overlay_ring_initialized = false;
        xSemaphoreGive(s_media_session.lock);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = playback_create_task(
        scene_background_task,
        "tts_overlay",
        PLAYBACK_SCENE_BACKGROUND_TASK_STACK,
        s_media_session.overlay_decoder,
        5,
        &s_media_session.overlay_decoder_task,
        0,
        &s_media_session.overlay_decoder->task_with_caps
    );
    if (ok != pdPASS) {
        media_session_free_decoder(&s_media_session.overlay_decoder);
        free(s_media_session.overlay_url);
        s_media_session.overlay_url = NULL;
        scene_pcm_ring_destroy(&s_media_session.overlay_ring);
        s_media_session.overlay_ring_initialized = false;
        xSemaphoreGive(s_media_session.lock);
        return ESP_ERR_NO_MEM;
    }

    s_media_session.overlay_pending = true;
    s_media_session.overlay_active = false;
    s_media_session.overlay_releasing = false;
    s_media_session.overlay_started_reported = false;
    ESP_LOGI(
        TAG,
        "overlay queued id=%s duck=%u%% attack=%ums release=%ums start=%" PRId64,
        s_media_session.overlay_id[0] ? s_media_session.overlay_id : "-",
        s_media_session.ducking_target_percent,
        s_media_session.ducking_attack_ms,
        s_media_session.ducking_release_ms,
        s_media_session.overlay_start_at_us
    );
    xSemaphoreGive(s_media_session.lock);
    return ESP_OK;
}

static esp_err_t play_wav_data_local(const uint8_t *data, size_t len, const char *label, bool free_data)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!playback_begin_start()) {
        return ESP_ERR_TIMEOUT;
    }

    playback_memory_args_t *request = calloc(1, sizeof(*request));
    if (!request) {
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    request->data = data;
    request->len = len;
    request->free_data = free_data;
    snprintf(request->label, sizeof(request->label), "%s", label ? label : "wake_sound");
    BaseType_t ok = playback_create_task(
        playback_memory_task,
        "tater_wake_wav",
        8192,
        request,
        5,
        &s_task,
        1,
        &request->task_with_caps
    );
    if (ok != pdPASS) {
        free(request);
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    playback_end_start();
    return ESP_OK;
}

esp_err_t tater_playback_play_wav_data_local(const uint8_t *data, size_t len, const char *label)
{
    return play_wav_data_local(data, len, label, false);
}

esp_err_t tater_playback_play_wav_data_owned_local(uint8_t *data, size_t len, const char *label)
{
    return play_wav_data_local(data, len, label, true);
}

static esp_err_t play_tone_async(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent, bool notify_finished)
{
    if (!playback_begin_start()) {
        return ESP_ERR_TIMEOUT;
    }

    tone_args_t *tone = calloc(1, sizeof(*tone));
    if (!tone) {
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    tone->frequency_hz = frequency_hz;
    tone->duration_ms = duration_ms;
    tone->volume_percent = volume_percent;
    tone->notify_finished = notify_finished;
    BaseType_t ok = playback_create_task(
        tone_task,
        "tater_tone",
        PLAYBACK_TONE_TASK_STACK,
        tone,
        5,
        &s_task,
        1,
        &tone->task_with_caps
    );
    if (ok != pdPASS) {
        free(tone);
        return playback_start_failed(ESP_ERR_NO_MEM);
    }
    playback_end_start();
    return ESP_OK;
}

esp_err_t tater_playback_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent)
{
    return play_tone_async(frequency_hz, duration_ms, volume_percent, true);
}

esp_err_t tater_playback_play_tone_local(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent)
{
    return play_tone_async(frequency_hz, duration_ms, volume_percent, false);
}

void tater_playback_stop(void)
{
    if (s_task || s_playing) {
        s_abort = true;
        for (int i = 0; i < 20 && s_playing; i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
}

bool tater_playback_is_playing(void)
{
    return s_playing;
}

bool tater_playback_media_session_active(void)
{
    if (!s_media_session.lock) {
        return false;
    }
    xSemaphoreTake(s_media_session.lock, portMAX_DELAY);
    bool active =
        s_media_session.active && s_media_session.accepting_overlays && !s_abort;
    xSemaphoreGive(s_media_session.lock);
    return active;
}
