#include "playback.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_i2s.h"
#include "board.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tater_protocol.h"

static const char *TAG = "tater_playback";

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
    uint32_t frequency_hz;
    uint32_t duration_ms;
    uint8_t volume_percent;
    bool notify_finished;
} tone_args_t;

typedef struct {
    char *url;
    bool notify_finished;
} playback_args_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    char label[64];
} playback_memory_args_t;

static volatile bool s_abort;
static volatile bool s_playing;
static TaskHandle_t s_task;

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

static uint8_t *alloc_audio(size_t size)
{
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buf;
}

static uint8_t *resize_audio(uint8_t *buf, size_t size)
{
    uint8_t *out = heap_caps_realloc(buf, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        out = heap_caps_realloc(buf, size, MALLOC_CAP_8BIT);
    }
    return out;
}

static esp_err_t fetch_url(const char *url, uint8_t **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "http client init failed");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    if (content_len > CONFIG_TATER_PLAYBACK_MAX_BYTES) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t capacity = content_len > 0 ? (size_t)content_len : 64 * 1024;
    if (capacity == 0) {
        capacity = 64 * 1024;
    }
    uint8_t *buf = alloc_audio(capacity);
    if (!buf) {
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    while (!s_abort) {
        if (used == capacity) {
            size_t next = capacity * 2;
            if (next > CONFIG_TATER_PLAYBACK_MAX_BYTES) {
                next = CONFIG_TATER_PLAYBACK_MAX_BYTES;
            }
            if (next <= capacity) {
                free(buf);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            uint8_t *grown = resize_audio(buf, next);
            if (!grown) {
                free(buf);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            buf = grown;
            capacity = next;
        }

        int got = esp_http_client_read(client, (char *)buf + used, capacity - used);
        if (got < 0) {
            free(buf);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (got == 0) {
            break;
        }
        used += (size_t)got;
    }

    esp_http_client_cleanup(client);
    if (s_abort || used == 0) {
        free(buf);
        return ESP_FAIL;
    }
    *out_buf = buf;
    *out_len = used;
    ESP_LOGI(TAG, "fetched playback audio bytes=%u", (unsigned)used);
    return ESP_OK;
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
            out[frames * 2] = left;
            out[(frames * 2) + 1] = right;
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
    uint8_t *buf = NULL;
    size_t len = 0;
    wav_info_t wav;

    s_playing = true;
    ESP_LOGI(TAG, "playback url=%s", url);
    tater_protocol_send_log("info", "playback started");

    esp_err_t err = fetch_url(url, &buf, &len);
    if (err == ESP_OK && !parse_wav(buf, len, &wav)) {
        ESP_LOGE(TAG, "unsupported wav");
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "wav rate=%" PRIu32 " channels=%u bits=%u frames=%u", wav.sample_rate, wav.channels, wav.bits_per_sample, (unsigned)wav.frame_count);
        err = play_wav(&wav);
    }

    if (buf) {
        free(buf);
    }
    if (notify_finished) {
        if (!s_abort && err == ESP_OK) {
            tater_protocol_send_playback_finished();
            tater_protocol_send_log("info", "playback finished");
        } else {
            tater_protocol_send_playback_finished_status(false, false);
            tater_protocol_send_log("warn", "playback stopped or failed");
        }
    } else if (!s_abort && err == ESP_OK) {
        tater_protocol_send_log("info", "local playback finished");
    } else {
        tater_protocol_send_log("warn", "local playback stopped or failed");
    }
    free(url);
    free(request);
    s_playing = false;
    s_abort = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

static void playback_memory_task(void *arg)
{
    playback_memory_args_t *request = (playback_memory_args_t *)arg;
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
    free(request);
    s_playing = false;
    s_abort = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

static void tone_task(void *arg)
{
    tone_args_t *tone = (tone_args_t *)arg;
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
    if (tone->notify_finished) {
        if (!s_abort && err == ESP_OK) {
            tater_protocol_send_playback_finished();
            tater_protocol_send_log("info", "tone finished");
        } else {
            tater_protocol_send_playback_finished_status(false, false);
            tater_protocol_send_log("warn", "tone stopped or failed");
        }
    } else if (!s_abort && err == ESP_OK) {
        tater_protocol_send_log("info", "local tone finished");
    } else {
        tater_protocol_send_log("warn", "local tone stopped or failed");
    }
    free(tone);
    s_playing = false;
    s_abort = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t tater_playback_init(void)
{
    s_abort = false;
    s_playing = false;
    s_task = NULL;
    return ESP_OK;
}

static esp_err_t play_url(const char *url, bool notify_finished)
{
    if (!url || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    tater_playback_stop();

    playback_args_t *request = calloc(1, sizeof(*request));
    if (!request) {
        return ESP_ERR_NO_MEM;
    }
    request->url = strdup(url);
    if (!request->url) {
        free(request);
        return ESP_ERR_NO_MEM;
    }
    request->notify_finished = notify_finished;
    s_abort = false;
    BaseType_t ok = xTaskCreatePinnedToCore(playback_task, "tater_playback", 8192, request, 5, &s_task, 1);
    if (ok != pdPASS) {
        free(request->url);
        free(request);
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
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

esp_err_t tater_playback_play_wav_data_local(const uint8_t *data, size_t len, const char *label)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    tater_playback_stop();

    playback_memory_args_t *request = calloc(1, sizeof(*request));
    if (!request) {
        return ESP_ERR_NO_MEM;
    }
    request->data = data;
    request->len = len;
    snprintf(request->label, sizeof(request->label), "%s", label ? label : "wake_sound");
    s_abort = false;
    BaseType_t ok = xTaskCreatePinnedToCore(playback_memory_task, "tater_wake_wav", 8192, request, 5, &s_task, 1);
    if (ok != pdPASS) {
        free(request);
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t play_tone_async(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent, bool notify_finished)
{
    tater_playback_stop();

    tone_args_t *tone = calloc(1, sizeof(*tone));
    if (!tone) {
        return ESP_ERR_NO_MEM;
    }
    tone->frequency_hz = frequency_hz;
    tone->duration_ms = duration_ms;
    tone->volume_percent = volume_percent;
    tone->notify_finished = notify_finished;
    s_abort = false;
    BaseType_t ok = xTaskCreatePinnedToCore(tone_task, "tater_tone", 4096, tone, 5, &s_task, 1);
    if (ok != pdPASS) {
        free(tone);
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
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
