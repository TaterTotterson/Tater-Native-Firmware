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
#include "native_settings.h"
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
    uint8_t partial_frame[8];
    size_t partial_len;
    size_t data_bytes_seen;
    uint64_t resample_accum;
    int16_t out[256 * TATER_SPK_CHANNELS];
    size_t out_frames;
    uint32_t input_frames;
    uint32_t output_frames;
} wav_stream_state_t;

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
    bool free_data;
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

static esp_err_t wav_stream_flush(wav_stream_state_t *state)
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

static esp_err_t wav_stream_emit_frame(const wav_stream_info_t *info, wav_stream_state_t *state, const uint8_t *frame)
{
    int16_t left = scale_output_sample(wav_frame_sample_s16(info, frame, 0));
    int16_t right = scale_output_sample(info->channels > 1 ? wav_frame_sample_s16(info, frame, 1) : left);
    state->input_frames++;
    state->resample_accum += TATER_SPK_SAMPLE_RATE;

    while (!s_abort && state->resample_accum >= info->sample_rate) {
        state->out[state->out_frames * 2] = left;
        state->out[(state->out_frames * 2) + 1] = right;
        state->out_frames++;
        state->resample_accum -= info->sample_rate;
        if (state->out_frames >= 256) {
            esp_err_t err = wav_stream_flush(state);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
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

static esp_err_t stream_wav_url(const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "http client init failed");

    uint8_t *read_buf = alloc_audio(8192);
    uint8_t *header_buf = alloc_audio(16 * 1024);
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
    wav_stream_info_t info = {0};
    wav_stream_state_t state = {0};
    size_t header_len = 0;
    bool started = false;
    bool header_ready = false;

    while (!s_abort) {
        int got = esp_http_client_read(client, (char *)read_buf, 8192);
        if (got < 0) {
            err = ESP_FAIL;
            goto done;
        }
        if (got == 0) {
            break;
        }

        if (!header_ready) {
            if (header_len + (size_t)got > 16 * 1024) {
                ESP_LOGE(TAG, "WAV header exceeded limit");
                err = ESP_ERR_NOT_SUPPORTED;
                goto done;
            }
            memcpy(header_buf + header_len, read_buf, (size_t)got);
            header_len += (size_t)got;

            size_t data_offset = 0;
            wav_header_result_t header = parse_wav_stream_header(header_buf, header_len, &info, &data_offset);
            if (header == WAV_HEADER_NEED_MORE) {
                continue;
            }
            if (header == WAV_HEADER_INVALID) {
                ESP_LOGE(TAG, "unsupported streamed wav");
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
            err = tater_audio_speaker_begin();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "speaker begin failed: %s", esp_err_to_name(err));
                goto done;
            }
            started = true;
            header_ready = true;
            err = wav_stream_process_data(&info, &state, header_buf + data_offset, header_len - data_offset);
            if (err != ESP_OK) {
                goto done;
            }
            continue;
        }

        err = wav_stream_process_data(&info, &state, read_buf, (size_t)got);
        if (err != ESP_OK) {
            goto done;
        }
        if (info.data_len > 0 && state.data_bytes_seen >= info.data_len) {
            break;
        }
    }

    if (!header_ready) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    if (!s_abort) {
        err = wav_stream_flush(&state);
    }
    ESP_LOGI(
        TAG,
        "stream wav playback input_frames=%u output_frames=%u bytes=%u",
        (unsigned)state.input_frames,
        (unsigned)state.output_frames,
        (unsigned)state.data_bytes_seen
    );

done:
    if (started) {
        esp_err_t end_err = tater_audio_speaker_end();
        if (end_err != ESP_OK) {
            ESP_LOGW(TAG, "speaker end failed err=%s", esp_err_to_name(end_err));
        }
    }
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

    s_playing = true;
    ESP_LOGI(TAG, "playback url=%s", url);
    tater_protocol_send_log("info", "playback started");

    esp_err_t err = stream_wav_url(url);
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
    if (request && request->free_data && request->data) {
        free((void *)request->data);
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

static esp_err_t play_wav_data_local(const uint8_t *data, size_t len, const char *label, bool free_data)
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
    request->free_data = free_data;
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
