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
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "native_settings.h"
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
static const uint32_t PLAYBACK_STOP_WAIT_MS = 3000;
static const uint32_t PLAYBACK_STOP_POLL_MS = 20;

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
    bool speaker_started;
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

static volatile bool s_abort;
static volatile bool s_playing;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lifecycle_lock;

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
    s_task = NULL;
    s_playing = false;
    s_abort = false;
    playback_end_start();
    return err;
}

static void playback_mark_finished(void)
{
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

static int16_t scale_output_sample(int16_t sample)
{
    const tater_live_settings_t *settings = tater_live_settings_get();
    uint8_t volume = settings ? settings->volume_percent : 100;
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
    esp_err_t err = tater_audio_write_speaker_frames(state->out, state->out_frames);
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

    left = scale_output_sample(left);
    right = scale_output_sample(right);
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

static esp_err_t codec_stream_begin(codec_stream_state_t *stream, stream_audio_type_t type)
{
    if (!stream) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(audio_codec_register_once(), TAG, "codec registry failed");
    memset(stream, 0, sizeof(*stream));
    stream->type = simple_decoder_type_for_stream(type);
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
    if (stream->speaker_started) {
        esp_err_t end_err = tater_audio_speaker_end();
        if (end_err != ESP_OK) {
            ESP_LOGW(TAG, "speaker end failed err=%s", esp_err_to_name(end_err));
        }
        stream->speaker_started = false;
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
        ESP_RETURN_ON_ERROR(tater_audio_speaker_begin(), TAG, "speaker begin failed");
        stream->speaker_started = true;
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
    int64_t content_length
)
{
    codec_jitter_buffer_t jitter = {0};
    codec_stream_state_t stream = {0};
    esp_err_t err = codec_stream_begin(&stream, type);
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
        if (stream.speaker_started) {
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
    const char *reason
)
{
    if (!client || !info || !read_buf || read_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (reason && reason[0]) {
        tater_protocol_send_log("warn", reason);
    }

    wav_stream_state_t state = {0};
    esp_err_t err = tater_audio_speaker_begin();
    bool speaker_started = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker begin failed: %s", esp_err_to_name(err));
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
        esp_err_t end_err = tater_audio_speaker_end();
        if (end_err != ESP_OK) {
            ESP_LOGW(TAG, "speaker end failed err=%s", esp_err_to_name(end_err));
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
    int64_t content_length
)
{
    if (!client || !info || !read_buf || read_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    codec_jitter_buffer_t jitter = {0};
    wav_stream_state_t state = {0};
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
            "wav buffered playback unavailable; using direct playback"
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
            "wav reader unavailable; using direct playback"
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
        err = tater_audio_speaker_begin();
        if (err == ESP_OK) {
            speaker_started = true;
        } else {
            ESP_LOGE(TAG, "speaker begin failed: %s", esp_err_to_name(err));
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
        esp_err_t end_err = tater_audio_speaker_end();
        if (end_err != ESP_OK) {
            ESP_LOGW(TAG, "speaker end failed err=%s", esp_err_to_name(end_err));
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

static esp_err_t stream_audio_url(const char *url)
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
                    content_length
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
                content_length
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

    ESP_RETURN_ON_ERROR(tater_audio_speaker_begin(), TAG, "speaker begin failed");
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
            out[frames * 2] = scale_output_sample(left);
            out[(frames * 2) + 1] = scale_output_sample(right);
            frames++;
            pos_q32 += step_q32;
        }
        if (frames == 0) {
            break;
        }
        esp_err_t err = tater_audio_write_speaker_frames(out, frames);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "speaker write failed err=%s", esp_err_to_name(err));
            result = err;
            break;
        }
        played_frames += frames;
    }
    ESP_LOGI(TAG, "wav playback wrote frames=%u", (unsigned)played_frames);
    esp_err_t end_err = tater_audio_speaker_end();
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

    ESP_RETURN_ON_ERROR(tater_audio_speaker_begin(), TAG, "speaker begin failed");

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
        esp_err_t err = tater_audio_write_speaker_frames(out, frames);
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
    esp_err_t end_err = tater_audio_speaker_end();
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

    esp_err_t err = stream_audio_url(url);
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

static void playback_memory_task(void *arg)
{
    playback_memory_args_t *request = (playback_memory_args_t *)arg;
    bool task_with_caps = request ? request->task_with_caps : false;
    wav_info_t wav;
    esp_err_t err = ESP_OK;

    s_playing = true;
    ESP_LOGI(TAG, "local wav playback label=%s bytes=%u", request ? request->label : "", (unsigned)(request ? request->len : 0));
    tater_protocol_send_log("info", "local wake sound started");

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

    if (!s_abort && err == ESP_OK) {
        tater_protocol_send_log("info", "local wake sound finished");
    } else {
        tater_protocol_send_log("warn", "local wake sound stopped or failed");
    }
    if (request && request->free_data && request->data) {
        free((void *)request->data);
    }
    free(request);
    playback_mark_finished();
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
        4096,
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
