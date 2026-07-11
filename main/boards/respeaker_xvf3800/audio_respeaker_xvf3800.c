#include "audio_i2s.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_aec.h"
#include "board.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "native_settings.h"
#include "tater_protocol.h"
#include "wake_engine.h"

#if TATER_BOARD_RESPEAKER_XVF3800

static const char *TAG = "tater_audio_xvf3800";

extern const uint8_t _binary_xvf3800_i2s_1_0_7_bin_start[] asm("_binary_xvf3800_i2s_1_0_7_bin_start");
extern const uint8_t _binary_xvf3800_i2s_1_0_7_bin_end[] asm("_binary_xvf3800_i2s_1_0_7_bin_end");

#define TATER_I2C_PORT I2C_NUM_0
#define XVF3800_I2C_ADDR 0x2C
#define XVF3800_I2C_TIMEOUT_MS 150

#define XVF_DFU_RESID 240
#define XVF_DFU_READ_BIT 0x80
#define XVF_DFU_DNLOAD 1
#define XVF_DFU_GETSTATUS 3
#define XVF_DFU_SETALTERNATE 64
#define XVF_DFU_GETVERSION 88
#define XVF_DFU_REBOOT 89
#define XVF_DFU_ALTERNATE_UPGRADE 1
#define XVF_DFU_STATE_IDLE 2
#define XVF_DFU_STATE_DNLOAD_IDLE 5
#define XVF_DFU_STATE_MANIFEST_WAIT_RESET 8
#define XVF_DFU_STATUS_OK 0
#define XVF_DFU_MAX_XFER 128
#define XVF_DFU_READY_TIMEOUT_MS 30000
#define XVF_DFU_VERIFY_TIMEOUT_MS 10000
#define XVF_VERSION_READY_TIMEOUT_MS 8000
#define XVF_VERSION_RETRY_DELAY_MS 250
#define XVF_TARGET_MAJOR 1
#define XVF_TARGET_MINOR 0
#define XVF_TARGET_PATCH 7

#define XVF_GPO_RESID 20
#define XVF_GPO_READ_VALUES 0
#define XVF_GPO_WRITE_VALUE 1
#define XVF_GPO_LED_RING_VALUE 18
#define XVF_GPO_PIN_MUTE 30
#define XVF_GPO_PIN_AMP_ENABLE 31
#define XVF_GPO_PIN_LED_POWER 33
#define XVF_GPO_COUNT 5

#define XVF_AEC_RESID 33
#define XVF_AEC_AZIMUTH_VALUES 75
#define XVF_AEC_AZIMUTH_RESPONSE_LEN 17

#define CTRL_DONE 0
#define CTRL_WAIT 1
#define CTRL_RETRY 0x40

#define RESPEAKER_XVF_WAKE_CHANNEL 1
#define RESPEAKER_XVF_STREAM_CHANNEL 1
#define RESPEAKER_XVF_MIC_GAIN_Q8 256
#define RESPEAKER_XVF_SPK_DMA_DESC_NUM 4
#define RESPEAKER_XVF_SPK_DMA_FRAME_NUM 240
#define RESPEAKER_XVF_SPK_WRITE_FRAMES RESPEAKER_XVF_SPK_DMA_FRAME_NUM
#define RESPEAKER_XVF_WAKE_DIAG_PEAK_THRESHOLD 96U
#define RESPEAKER_XVF_WAKE_DIAG_INTERVAL_US 2000000

static i2s_chan_handle_t s_rx_chan;
static i2s_chan_handle_t s_tx_chan;
static SemaphoreHandle_t s_i2c_mutex;
static SemaphoreHandle_t s_speaker_mutex;
static bool s_i2c_ready;
static bool s_speaker_ready;
static bool s_speaker_enabled;
static bool s_speaker_primed;
static bool s_speaker_session_active;
static bool s_xvf_muted;
static portMUX_TYPE s_speaker_level_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_speaker_audio_level;
static int64_t s_speaker_level_update_us;
static portMUX_TYPE s_doa_lock = portMUX_INITIALIZER_UNLOCKED;
static tater_audio_doa_t s_doa;
static int64_t s_doa_update_us;
static uint32_t s_doa_failed_reads;
static portMUX_TYPE s_xmos_status_lock = portMUX_INITIALIZER_UNLOCKED;
static tater_audio_xmos_status_t s_xmos_status = {
    .target_major = XVF_TARGET_MAJOR,
    .target_minor = XVF_TARGET_MINOR,
    .target_patch = XVF_TARGET_PATCH,
    .update_state = TATER_XMOS_UPDATE_IDLE,
};

static size_t xvf_target_firmware_size(void)
{
    return (size_t)(_binary_xvf3800_i2s_1_0_7_bin_end - _binary_xvf3800_i2s_1_0_7_bin_start);
}

static uint32_t read_u24_le(const uint8_t *payload)
{
    return (uint32_t)payload[0]
        | ((uint32_t)payload[1] << 8)
        | ((uint32_t)payload[2] << 16);
}

static esp_err_t speaker_session_take(void)
{
    if (!s_speaker_mutex) {
        s_speaker_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_speaker_mutex, ESP_ERR_NO_MEM, TAG, "speaker mutex failed");
    }
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_speaker_mutex, pdMS_TO_TICKS(5000)) == pdTRUE,
        ESP_ERR_TIMEOUT,
        TAG,
        "speaker session lock timeout"
    );
    return ESP_OK;
}

static void speaker_session_give(void)
{
    if (s_speaker_mutex && s_speaker_session_active) {
        s_speaker_session_active = false;
        xSemaphoreGive(s_speaker_mutex);
    }
}

static void xmos_status_set_update_state(tater_audio_xmos_update_state_t state)
{
    portENTER_CRITICAL(&s_xmos_status_lock);
    s_xmos_status.update_state = state;
    portEXIT_CRITICAL(&s_xmos_status_lock);
}

static void xmos_status_set_version(bool valid, uint8_t major, uint8_t minor, uint8_t patch)
{
    portENTER_CRITICAL(&s_xmos_status_lock);
    s_xmos_status.version_valid = valid;
    s_xmos_status.major = major;
    s_xmos_status.minor = minor;
    s_xmos_status.patch = patch;
    portEXIT_CRITICAL(&s_xmos_status_lock);
}

static void xmos_status_set_progress(uint8_t progress_percent)
{
    if (progress_percent > 100) {
        progress_percent = 100;
    }
    portENTER_CRITICAL(&s_xmos_status_lock);
    s_xmos_status.progress_percent = progress_percent;
    portEXIT_CRITICAL(&s_xmos_status_lock);
}

static void xmos_status_set_dfu(uint8_t dfu_state, uint8_t dfu_status)
{
    portENTER_CRITICAL(&s_xmos_status_lock);
    s_xmos_status.dfu_state = dfu_state;
    s_xmos_status.dfu_status = dfu_status;
    portEXIT_CRITICAL(&s_xmos_status_lock);
}

static void xmos_status_set_update_flags(bool attempted, bool required)
{
    portENTER_CRITICAL(&s_xmos_status_lock);
    s_xmos_status.update_attempted = attempted;
    s_xmos_status.update_required = required;
    portEXIT_CRITICAL(&s_xmos_status_lock);
}

static bool xvf_target_version_matches(uint8_t major, uint8_t minor, uint8_t patch)
{
    return major == XVF_TARGET_MAJOR && minor == XVF_TARGET_MINOR && patch == XVF_TARGET_PATCH;
}

esp_err_t tater_audio_xvf3800_control_init(void)
{
    if (!s_i2c_mutex) {
        s_i2c_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_i2c_mutex, ESP_ERR_NO_MEM, TAG, "i2c mutex alloc failed");
    }
    if (s_i2c_ready) {
        return ESP_OK;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TATER_I2C_SDA,
        .scl_io_num = TATER_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {
            .clk_speed = 100000,
        },
        .clk_flags = 0,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(TATER_I2C_PORT, &conf), TAG, "i2c config failed");
    esp_err_t err = i2c_driver_install(TATER_I2C_PORT, conf.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_i2c_ready = true;
    ESP_LOGI(TAG, "xvf3800 i2c ready addr=0x%02x sda=%d scl=%d", XVF3800_I2C_ADDR, TATER_I2C_SDA, TATER_I2C_SCL);
    return ESP_OK;
}

static esp_err_t locked_i2c_write(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_ERROR(tater_audio_xvf3800_control_init(), TAG, "i2c init failed");
    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    esp_err_t err = i2c_master_write_to_device(
        TATER_I2C_PORT,
        XVF3800_I2C_ADDR,
        data,
        len,
        pdMS_TO_TICKS(XVF3800_I2C_TIMEOUT_MS)
    );
    xSemaphoreGive(s_i2c_mutex);
    return err;
}

static esp_err_t locked_i2c_write_read(const uint8_t *write_data, size_t write_len, uint8_t *read_data, size_t read_len)
{
    ESP_RETURN_ON_ERROR(tater_audio_xvf3800_control_init(), TAG, "i2c init failed");
    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    esp_err_t err = i2c_master_write_read_device(
        TATER_I2C_PORT,
        XVF3800_I2C_ADDR,
        write_data,
        write_len,
        read_data,
        read_len,
        pdMS_TO_TICKS(XVF3800_I2C_TIMEOUT_MS)
    );
    xSemaphoreGive(s_i2c_mutex);
    return err;
}

static esp_err_t xvf_write_bytes(uint8_t resid, uint8_t cmd, const uint8_t *value, uint8_t write_byte_num)
{
    uint8_t payload[3 + 255] = {0};
    payload[0] = resid;
    payload[1] = cmd;
    payload[2] = write_byte_num;
    if (value && write_byte_num > 0) {
        memcpy(&payload[3], value, write_byte_num);
    }
    return locked_i2c_write(payload, 3U + write_byte_num);
}

static esp_err_t xvf_gpo_write(uint8_t pin, bool high)
{
    uint8_t payload[] = {pin, high ? 1 : 0};
    return xvf_write_bytes(XVF_GPO_RESID, XVF_GPO_WRITE_VALUE, payload, sizeof(payload));
}

static esp_err_t xvf_read_gpo(uint8_t out[XVF_GPO_COUNT])
{
    const uint8_t request[] = {XVF_GPO_RESID, XVF_GPO_READ_VALUES | 0x80, XVF_GPO_COUNT + 1};
    uint8_t response[XVF_GPO_COUNT + 1] = {0};
    ESP_RETURN_ON_ERROR(locked_i2c_write_read(request, sizeof(request), response, sizeof(response)), TAG, "gpo read failed");
    ESP_RETURN_ON_FALSE(response[0] == CTRL_DONE, ESP_FAIL, TAG, "gpo read status=0x%02x", response[0]);
    memcpy(out, &response[1], XVF_GPO_COUNT);
    return ESP_OK;
}

esp_err_t tater_audio_xvf3800_set_mute(bool muted)
{
    esp_err_t err = xvf_gpo_write(XVF_GPO_PIN_MUTE, muted);
    if (err == ESP_OK) {
        s_xvf_muted = muted;
    } else {
        ESP_LOGW(TAG, "xvf mute write failed muted=%d err=%s", muted, esp_err_to_name(err));
    }
    return err;
}

esp_err_t tater_audio_xvf3800_read_mute(bool *muted)
{
    if (!muted) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t gpo[XVF_GPO_COUNT] = {0};
    esp_err_t err = xvf_read_gpo(gpo);
    if (err != ESP_OK) {
        *muted = s_xvf_muted;
        return err;
    }
    *muted = (gpo[1] & 0x01) != 0;
    s_xvf_muted = *muted;
    return ESP_OK;
}

esp_err_t tater_audio_xvf3800_set_led_ring(const uint8_t *rgb, size_t led_count)
{
    if (!rgb || led_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t payload[TATER_LED_COUNT * 4] = {0};
    size_t count = led_count < TATER_LED_COUNT ? led_count : TATER_LED_COUNT;
    for (size_t i = 0; i < count; i++) {
        const uint8_t r = rgb[(i * 3) + 0];
        const uint8_t g = rgb[(i * 3) + 1];
        const uint8_t b = rgb[(i * 3) + 2];
        payload[(i * 4) + 0] = b;
        payload[(i * 4) + 1] = g;
        payload[(i * 4) + 2] = r;
        payload[(i * 4) + 3] = 0x00;
    }
    return xvf_write_bytes(XVF_GPO_RESID, XVF_GPO_LED_RING_VALUE, payload, sizeof(payload));
}

static esp_err_t xvf_read_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
    const uint8_t request[] = {XVF_DFU_RESID, XVF_DFU_GETVERSION | XVF_DFU_READ_BIT, 4};
    uint8_t response[4] = {0};
    ESP_RETURN_ON_ERROR(locked_i2c_write_read(request, sizeof(request), response, sizeof(response)), TAG, "version read failed");
    ESP_RETURN_ON_FALSE(response[0] == CTRL_DONE, ESP_FAIL, TAG, "version status=0x%02x", response[0]);
    if (major) {
        *major = response[1];
    }
    if (minor) {
        *minor = response[2];
    }
    if (patch) {
        *patch = response[3];
    }
    xmos_status_set_version(true, response[1], response[2], response[3]);
    ESP_LOGI(TAG, "xvf3800 firmware version %u.%u.%u", response[1], response[2], response[3]);
    return ESP_OK;
}

static esp_err_t xvf_read_version_with_retry(uint8_t *major, uint8_t *minor, uint8_t *patch, uint32_t timeout_ms, const char *phase)
{
    int64_t deadline = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    esp_err_t last_err = ESP_FAIL;
    uint32_t attempts = 0;
    while (esp_timer_get_time() < deadline) {
        attempts++;
        last_err = xvf_read_version(major, minor, patch);
        if (last_err == ESP_OK) {
            ESP_LOGI(TAG, "xvf3800 version ready during %s after %" PRIu32 " attempt(s)", phase ? phase : "startup", attempts);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(XVF_VERSION_RETRY_DELAY_MS));
    }
    xmos_status_set_version(false, 0, 0, 0);
    ESP_LOGW(TAG, "xvf3800 version unavailable during %s err=%s", phase ? phase : "startup", esp_err_to_name(last_err));
    return last_err;
}

static esp_err_t xvf_dfu_get_status(uint8_t *dfu_state, uint8_t *dfu_status, uint32_t *next_delay_ms)
{
    const uint8_t request[] = {XVF_DFU_RESID, XVF_DFU_GETSTATUS | XVF_DFU_READ_BIT, 6};
    uint8_t response[6] = {0};
    ESP_RETURN_ON_ERROR(locked_i2c_write_read(request, sizeof(request), response, sizeof(response)), TAG, "dfu status read failed");
    ESP_RETURN_ON_FALSE(response[0] == CTRL_DONE, ESP_FAIL, TAG, "dfu status=0x%02x", response[0]);
    uint8_t status = response[1];
    uint8_t state = response[5];
    uint32_t delay_ms = read_u24_le(&response[2]);
    if (dfu_state) {
        *dfu_state = state;
    }
    if (dfu_status) {
        *dfu_status = status;
    }
    if (next_delay_ms) {
        *next_delay_ms = delay_ms;
    }
    xmos_status_set_dfu(state, status);
    ESP_RETURN_ON_FALSE(status == XVF_DFU_STATUS_OK, ESP_FAIL, TAG, "dfu status non-ok status=%u state=%u", status, state);
    return ESP_OK;
}

static bool xvf_dfu_ready_state(uint8_t state)
{
    return state == XVF_DFU_STATE_IDLE
        || state == XVF_DFU_STATE_DNLOAD_IDLE
        || state == XVF_DFU_STATE_MANIFEST_WAIT_RESET;
}

static esp_err_t xvf_dfu_wait_ready(const char *phase, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() < deadline) {
        uint8_t state = 0;
        uint8_t status = 0;
        uint32_t delay_ms = 0;
        esp_err_t err = xvf_dfu_get_status(&state, &status, &delay_ms);
        if (err == ESP_OK && xvf_dfu_ready_state(state)) {
            return ESP_OK;
        }
        if (delay_ms > 250) {
            delay_ms = 250;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms ? delay_ms : 10));
    }
    ESP_LOGE(TAG, "xvf3800 dfu wait timeout phase=%s", phase ? phase : "ready");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t xvf_dfu_set_alternate(void)
{
    const uint8_t request[] = {XVF_DFU_RESID, XVF_DFU_SETALTERNATE, 1, XVF_DFU_ALTERNATE_UPGRADE};
    return locked_i2c_write(request, sizeof(request));
}

static esp_err_t xvf_dfu_reboot(void)
{
    const uint8_t request[] = {XVF_DFU_RESID, XVF_DFU_REBOOT, 1, 0};
    return locked_i2c_write(request, sizeof(request));
}

static esp_err_t xvf_dfu_update_to_target(void)
{
    const uint8_t *image = _binary_xvf3800_i2s_1_0_7_bin_start;
    const size_t image_size = xvf_target_firmware_size();
    ESP_RETURN_ON_FALSE(image && image_size > 0, ESP_ERR_INVALID_SIZE, TAG, "xvf target image missing");

    xmos_status_set_update_flags(true, true);
    xmos_status_set_update_state(TATER_XMOS_UPDATE_RUNNING);
    xmos_status_set_progress(0);
    ESP_LOGW(TAG, "xvf3800 firmware update starting target=%u.%u.%u size=%u", XVF_TARGET_MAJOR, XVF_TARGET_MINOR, XVF_TARGET_PATCH, (unsigned)image_size);
    ESP_RETURN_ON_ERROR(xvf_dfu_set_alternate(), TAG, "dfu set alternate failed");

    size_t offset = 0;
    uint8_t last_progress = 0;
    while (offset < image_size) {
        ESP_RETURN_ON_ERROR(xvf_dfu_wait_ready("download", XVF_DFU_READY_TIMEOUT_MS), TAG, "dfu download wait failed");
        size_t chunk = image_size - offset;
        if (chunk > XVF_DFU_MAX_XFER) {
            chunk = XVF_DFU_MAX_XFER;
        }
        uint8_t request[5 + XVF_DFU_MAX_XFER] = {0};
        request[0] = XVF_DFU_RESID;
        request[1] = XVF_DFU_DNLOAD;
        request[2] = XVF_DFU_MAX_XFER + 2;
        request[3] = (uint8_t)chunk;
        request[4] = 0;
        memcpy(&request[5], image + offset, chunk);
        ESP_RETURN_ON_ERROR(locked_i2c_write(request, sizeof(request)), TAG, "dfu download block failed");
        offset += chunk;
        uint8_t progress = (uint8_t)((offset * 100U) / image_size);
        if (progress != last_progress && (progress == 100 || progress >= (uint8_t)(last_progress + 5))) {
            last_progress = progress;
            xmos_status_set_progress(progress);
            ESP_LOGI(TAG, "xvf3800 firmware update progress=%u%%", progress);
        }
    }

    ESP_RETURN_ON_ERROR(xvf_dfu_wait_ready("finalize", XVF_DFU_READY_TIMEOUT_MS), TAG, "dfu finalize wait failed");
    uint8_t final_request[5 + XVF_DFU_MAX_XFER] = {0};
    final_request[0] = XVF_DFU_RESID;
    final_request[1] = XVF_DFU_DNLOAD;
    final_request[2] = XVF_DFU_MAX_XFER + 2;
    ESP_RETURN_ON_ERROR(locked_i2c_write(final_request, sizeof(final_request)), TAG, "dfu final download failed");
    ESP_RETURN_ON_ERROR(xvf_dfu_wait_ready("manifest", XVF_DFU_READY_TIMEOUT_MS), TAG, "dfu manifest wait failed");
    ESP_RETURN_ON_ERROR(xvf_dfu_reboot(), TAG, "dfu reboot failed");

    uint8_t major = 0;
    uint8_t minor = 0;
    uint8_t patch = 0;
    ESP_RETURN_ON_ERROR(xvf_read_version_with_retry(&major, &minor, &patch, XVF_DFU_VERIFY_TIMEOUT_MS, "dfu verify"), TAG, "dfu version verify failed");
    ESP_RETURN_ON_FALSE(xvf_target_version_matches(major, minor, patch), ESP_FAIL, TAG, "dfu verify mismatch installed=%u.%u.%u", major, minor, patch);
    xmos_status_set_update_flags(true, false);
    xmos_status_set_progress(100);
    xmos_status_set_dfu(0, 0);
    xmos_status_set_update_state(TATER_XMOS_UPDATE_COMPLETE);
    ESP_LOGI(TAG, "xvf3800 firmware update complete");
    return ESP_OK;
}

static esp_err_t xvf_ensure_target_firmware(void)
{
    uint8_t major = 0;
    uint8_t minor = 0;
    uint8_t patch = 0;
    esp_err_t err = xvf_read_version_with_retry(&major, &minor, &patch, XVF_VERSION_READY_TIMEOUT_MS, "startup");
    if (err == ESP_OK && xvf_target_version_matches(major, minor, patch)) {
        xmos_status_set_update_flags(false, false);
        xmos_status_set_progress(100);
        xmos_status_set_update_state(TATER_XMOS_UPDATE_SKIPPED);
        ESP_LOGI(TAG, "xvf3800 target firmware already installed");
        return ESP_OK;
    }
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "xvf3800 firmware mismatch installed=%u.%u.%u target=%u.%u.%u", major, minor, patch, XVF_TARGET_MAJOR, XVF_TARGET_MINOR, XVF_TARGET_PATCH);
    } else {
        ESP_LOGW(TAG, "xvf3800 firmware version unavailable; attempting I2C DFU target=%u.%u.%u", XVF_TARGET_MAJOR, XVF_TARGET_MINOR, XVF_TARGET_PATCH);
    }
    err = xvf_dfu_update_to_target();
    if (err != ESP_OK) {
        xmos_status_set_update_state(TATER_XMOS_UPDATE_ERROR);
        ESP_LOGE(TAG, "xvf3800 firmware update failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t xvf_read_doa(tater_audio_doa_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t request[] = {XVF_AEC_RESID, XVF_AEC_AZIMUTH_VALUES | 0x80, XVF_AEC_AZIMUTH_RESPONSE_LEN};
    uint8_t response[XVF_AEC_AZIMUTH_RESPONSE_LEN] = {0};
    ESP_RETURN_ON_ERROR(locked_i2c_write_read(request, sizeof(request), response, sizeof(response)), TAG, "azimuth read failed");
    if (response[0] == CTRL_WAIT || response[0] == CTRL_RETRY) {
        return ESP_ERR_TIMEOUT;
    }
    ESP_RETURN_ON_FALSE(response[0] == CTRL_DONE, ESP_FAIL, TAG, "azimuth status=0x%02x", response[0]);
    float radians = 0.0f;
    memcpy(&radians, &response[1 + (3 * sizeof(float))], sizeof(radians));
    if (!isfinite(radians)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    float degrees = radians * 180.0f / (float)M_PI;
    while (degrees < 0.0f) {
        degrees += 360.0f;
    }
    while (degrees >= 360.0f) {
        degrees -= 360.0f;
    }
    int angle_index = (int)((degrees / 360.0f) * (float)TATER_LED_COUNT + 0.5f) % TATER_LED_COUNT;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->four_mic = true;
    out->confidence = 12;
    out->angle_index = (uint8_t)angle_index;
    out->energy = 1;
    return ESP_OK;
}

static void doa_task(void *arg)
{
    (void)arg;
    while (true) {
        tater_audio_doa_t snapshot = {0};
        esp_err_t err = xvf_read_doa(&snapshot);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_doa_lock);
            s_doa = snapshot;
            s_doa_update_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_doa_lock);
            s_doa_failed_reads = 0;
        } else if (err != ESP_ERR_TIMEOUT) {
            s_doa_failed_reads++;
            if ((s_doa_failed_reads % 50U) == 1U) {
                ESP_LOGD(TAG, "xvf3800 doa read failed count=%lu err=%s", (unsigned long)s_doa_failed_reads, esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t xvf_configure_runtime_gpo(void)
{
    ESP_RETURN_ON_ERROR(xvf_gpo_write(XVF_GPO_PIN_LED_POWER, true), TAG, "led power enable failed");
    ESP_RETURN_ON_ERROR(xvf_gpo_write(XVF_GPO_PIN_AMP_ENABLE, false), TAG, "amp enable failed");
    const tater_live_settings_t *settings = tater_live_settings_get();
    ESP_RETURN_ON_ERROR(tater_audio_xvf3800_set_mute(settings ? settings->muted : false), TAG, "mute sync failed");
    return ESP_OK;
}

static esp_err_t i2s_init_duplex(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num = RESPEAKER_XVF_SPK_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = RESPEAKER_XVF_SPK_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;
    chan_cfg.intr_priority = 3;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan), TAG, "i2s_new_channel duplex failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = TATER_MIC_SOURCE_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = TATER_MIC_I2S_BCLK,
            .ws = TATER_MIC_I2S_WS,
            .dout = TATER_SPK_I2S_DOUT,
            .din = TATER_MIC_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg), TAG, "i2s tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "i2s rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "i2s rx enable failed");
    s_speaker_ready = true;
    ESP_LOGI(
        TAG,
        "xvf3800 i2s slave duplex ready bclk=%d ws=%d din=%d dout=%d",
        TATER_MIC_I2S_BCLK,
        TATER_MIC_I2S_WS,
        TATER_MIC_I2S_DIN,
        TATER_SPK_I2S_DOUT
    );
    return ESP_OK;
}

esp_err_t tater_audio_i2s_init(void)
{
    ESP_RETURN_ON_ERROR(tater_audio_xvf3800_control_init(), TAG, "control init failed");
    ESP_ERROR_CHECK_WITHOUT_ABORT(xvf_ensure_target_firmware());
    ESP_ERROR_CHECK_WITHOUT_ABORT(xvf_configure_runtime_gpo());
    ESP_RETURN_ON_ERROR(i2s_init_duplex(), TAG, "i2s init failed");
    tater_audio_aec_init();
    return ESP_OK;
}

static int16_t clamp_s16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static int16_t pcm32_to_pcm16_gain(int32_t sample)
{
    int32_t v = sample >> 16;
    v = (v * RESPEAKER_XVF_MIC_GAIN_Q8) / 256;
    return clamp_s16(v);
}

static int16_t xvf_channel_48k_to_16k(const int32_t *samples, size_t source_frame, size_t channel)
{
    int32_t sum = 0;
    if (channel >= TATER_MIC_SOURCE_CHANNELS) {
        channel = 0;
    }
    for (size_t frame = 0; frame < 3; frame++) {
        size_t base = (source_frame + frame) * TATER_MIC_SOURCE_CHANNELS;
        sum += pcm32_to_pcm16_gain(samples[base + channel]);
    }
    return clamp_s16(sum / 3);
}

static void mic_level_stats(const int16_t *samples, size_t count, uint32_t *peak_out, uint32_t *mean_out)
{
    uint32_t peak = 0;
    uint64_t sum = 0;
    if (samples) {
        for (size_t i = 0; i < count; i++) {
            int32_t sample = samples[i];
            uint32_t magnitude = sample < 0 ? (uint32_t)(-sample) : (uint32_t)sample;
            if (magnitude > peak) {
                peak = magnitude;
            }
            sum += magnitude;
        }
    }
    if (peak_out) {
        *peak_out = peak;
    }
    if (mean_out) {
        *mean_out = count ? (uint32_t)(sum / count) : 0;
    }
}

static float clamp_audio_level(float level)
{
    if (level <= 0.0f) {
        return 0.0f;
    }
    if (level >= 1.0f) {
        return 1.0f;
    }
    return level;
}

static void reset_speaker_audio_level(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_speaker_level_lock);
    s_speaker_audio_level = 0.0f;
    s_speaker_level_update_us = now;
    portEXIT_CRITICAL(&s_speaker_level_lock);
}

static void update_speaker_audio_level(const int16_t *stereo_frames, size_t frame_count)
{
    const size_t sample_count = frame_count * TATER_SPK_CHANNELS;
    if (!stereo_frames || sample_count == 0) {
        return;
    }
    uint32_t peak = 0;
    uint64_t sum_abs = 0;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = stereo_frames[i];
        uint32_t magnitude = sample < 0 ? (uint32_t)(-sample) : (uint32_t)sample;
        if (magnitude > peak) {
            peak = magnitude;
        }
        sum_abs += magnitude;
    }
    float mean_level = ((float)sum_abs / (float)sample_count) / 32768.0f;
    float peak_level = (float)peak / 32768.0f;
    float target = clamp_audio_level((peak_level * 0.75f) + (mean_level * 1.25f));
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_speaker_level_lock);
    float current = s_speaker_audio_level;
    float alpha = target > current ? 0.65f : 0.28f;
    s_speaker_audio_level = current + ((target - current) * alpha);
    s_speaker_level_update_us = now;
    portEXIT_CRITICAL(&s_speaker_level_lock);
}

static void audio_task(void *arg)
{
    (void)arg;
    int32_t rx[TATER_MIC_SOURCE_CHUNK_FRAMES * TATER_MIC_SOURCE_CHANNELS];
    int16_t wake_mono[TATER_MIC_CHUNK_FRAMES];
    int16_t stream_mono[TATER_MIC_CHUNK_FRAMES];
    uint32_t active_read_errors = 0;
    uint32_t active_chunks = 0;
    int64_t last_wake_diag_us = 0;
    bool last_active = false;

    while (true) {
        bool active = tater_protocol_voice_active() && tater_protocol_is_connected();
        if (active && !last_active) {
            active_read_errors = 0;
            active_chunks = 0;
            ESP_LOGI(TAG, "xvf3800 mic stream active; waiting for i2s frames");
        }
        last_active = active;

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, rx, sizeof(rx), &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) {
            if (active && (++active_read_errors % 10) == 0) {
                ESP_LOGW(TAG, "xvf3800 mic i2s read waiting err=%s bytes=%u", esp_err_to_name(err), (unsigned)bytes_read);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t source_frames = bytes_read / (sizeof(int32_t) * TATER_MIC_SOURCE_CHANNELS);
        if (source_frames > TATER_MIC_SOURCE_CHUNK_FRAMES) {
            source_frames = TATER_MIC_SOURCE_CHUNK_FRAMES;
        }
        size_t out_frames = source_frames / 3;
        if (out_frames > TATER_MIC_CHUNK_FRAMES) {
            out_frames = TATER_MIC_CHUNK_FRAMES;
        }

        for (size_t i = 0; i < out_frames; i++) {
            wake_mono[i] = xvf_channel_48k_to_16k(rx, i * 3, RESPEAKER_XVF_WAKE_CHANNEL);
            stream_mono[i] = xvf_channel_48k_to_16k(rx, i * 3, RESPEAKER_XVF_STREAM_CHANNEL);
        }

        uint32_t wake_peak = 0;
        uint32_t wake_mean = 0;
        uint32_t stream_peak = 0;
        uint32_t stream_mean = 0;
        mic_level_stats(wake_mono, out_frames, &wake_peak, &wake_mean);
        mic_level_stats(stream_mono, out_frames, &stream_peak, &stream_mean);

        tater_wake_engine_note_audio(wake_mono, out_frames);
        if (!active) {
            tater_wake_engine_process(wake_mono, out_frames);
            int64_t now_us = esp_timer_get_time();
            if (wake_peak >= RESPEAKER_XVF_WAKE_DIAG_PEAK_THRESHOLD && now_us - last_wake_diag_us >= RESPEAKER_XVF_WAKE_DIAG_INTERVAL_US) {
                ESP_LOGD(
                    TAG,
                    "xvf3800 wake mic levels peak=%u mean=%u stream_peak=%u stream_mean=%u channel=%u",
                    (unsigned)wake_peak,
                    (unsigned)wake_mean,
                    (unsigned)stream_peak,
                    (unsigned)stream_mean,
                    (unsigned)RESPEAKER_XVF_WAKE_CHANNEL
                );
                last_wake_diag_us = now_us;
            }
        }

        if (active) {
            tater_audio_aec_process_mic(stream_mono, out_frames);
            if (active_chunks < 3) {
                ESP_LOGI(
                    TAG,
                    "xvf3800 mic chunk %u frames=%u source_frames=%u bytes=%u peak=%u mean=%u channel=%u",
                    (unsigned)(active_chunks + 1),
                    (unsigned)out_frames,
                    (unsigned)source_frames,
                    (unsigned)bytes_read,
                    (unsigned)stream_peak,
                    (unsigned)stream_mean,
                    (unsigned)RESPEAKER_XVF_STREAM_CHANNEL
                );
            }
            active_chunks++;
            tater_protocol_send_audio(stream_mono, out_frames);
        }
    }
}

void tater_audio_i2s_start_task(void)
{
    if (!s_speaker_mutex) {
        s_speaker_mutex = xSemaphoreCreateMutex();
    }
    xTaskCreatePinnedToCore(audio_task, "tater_audio", 8192, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(doa_task, "tater_doa", 4096, NULL, 4, NULL, 0);
}

esp_err_t tater_audio_speaker_begin(void)
{
    ESP_RETURN_ON_ERROR(speaker_session_take(), TAG, "speaker session lock failed");
    if (!s_tx_chan) {
        xSemaphoreGive(s_speaker_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    reset_speaker_audio_level();
    ESP_ERROR_CHECK_WITHOUT_ABORT(xvf_gpo_write(XVF_GPO_PIN_AMP_ENABLE, false));
    if (s_speaker_enabled) {
        esp_err_t err = i2s_channel_disable(s_tx_chan);
        if (err != ESP_OK) {
            xSemaphoreGive(s_speaker_mutex);
            ESP_LOGE(TAG, "speaker i2s disable failed: %s", esp_err_to_name(err));
            return err;
        }
        s_speaker_enabled = false;
    }
    esp_err_t err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        xSemaphoreGive(s_speaker_mutex);
        ESP_LOGE(TAG, "speaker i2s enable failed: %s", esp_err_to_name(err));
        return err;
    }
    s_speaker_enabled = true;
    s_speaker_primed = false;
    s_speaker_session_active = true;
    return ESP_OK;
}

static void speaker_prime_silence(void)
{
    if (!s_tx_chan || s_speaker_primed) {
        return;
    }
    int32_t zeros[RESPEAKER_XVF_SPK_WRITE_FRAMES * TATER_SPK_CHANNELS] = {0};
    size_t byte_count = sizeof(zeros);
    for (uint8_t i = 0; i < RESPEAKER_XVF_SPK_DMA_DESC_NUM; i++) {
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, zeros, byte_count, &bytes_written, pdMS_TO_TICKS(2));
        if (err != ESP_OK || bytes_written != byte_count) {
            ESP_LOGD(TAG, "speaker prime short err=%s bytes=%u/%u", esp_err_to_name(err), (unsigned)bytes_written, (unsigned)byte_count);
            break;
        }
    }
    s_speaker_primed = true;
}

esp_err_t tater_audio_write_speaker_frames(const int16_t *stereo_frames, size_t frame_count)
{
    if (!s_tx_chan || !stereo_frames || frame_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    update_speaker_audio_level(stereo_frames, frame_count);
    tater_audio_aec_note_speaker_frames(stereo_frames, frame_count);
    speaker_prime_silence();

    size_t offset = 0;
    while (offset < frame_count) {
        size_t frames = frame_count - offset;
        if (frames > RESPEAKER_XVF_SPK_WRITE_FRAMES) {
            frames = RESPEAKER_XVF_SPK_WRITE_FRAMES;
        }
        int32_t tx[RESPEAKER_XVF_SPK_WRITE_FRAMES * TATER_SPK_CHANNELS] = {0};
        for (size_t i = 0; i < frames * TATER_SPK_CHANNELS; i++) {
            tx[i] = ((int32_t)stereo_frames[(offset * TATER_SPK_CHANNELS) + i]) << 16;
        }
        size_t byte_count = frames * TATER_SPK_CHANNELS * sizeof(int32_t);
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, tx, byte_count, &bytes_written, pdMS_TO_TICKS(35));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "speaker i2s write failed err=%s bytes=%u/%u", esp_err_to_name(err), (unsigned)bytes_written, (unsigned)byte_count);
            return err;
        }
        if (bytes_written != byte_count) {
            ESP_LOGW(TAG, "speaker i2s write short bytes=%u/%u", (unsigned)bytes_written, (unsigned)byte_count);
            return ESP_ERR_TIMEOUT;
        }
        offset += frames;
    }
    return ESP_OK;
}

esp_err_t tater_audio_speaker_end(void)
{
    if (!s_tx_chan) {
        return ESP_OK;
    }
    esp_err_t result = ESP_OK;
    if (s_speaker_enabled) {
        esp_err_t err = i2s_channel_disable(s_tx_chan);
        if (err != ESP_OK) {
            result = err;
        }
    }
    s_speaker_enabled = false;
    s_speaker_primed = false;
    reset_speaker_audio_level();
    speaker_session_give();
    return result;
}

bool tater_audio_speaker_ready(void)
{
    return s_speaker_ready;
}

float tater_audio_speaker_level(void)
{
    int64_t now = esp_timer_get_time();
    float level = 0.0f;
    int64_t updated_us = 0;
    portENTER_CRITICAL(&s_speaker_level_lock);
    level = s_speaker_audio_level;
    updated_us = s_speaker_level_update_us;
    portEXIT_CRITICAL(&s_speaker_level_lock);
    if (updated_us == 0 || now <= updated_us) {
        return clamp_audio_level(level);
    }
    int64_t age_us = now - updated_us;
    if (age_us >= 300000) {
        return 0.0f;
    }
    if (age_us > 80000) {
        float fade = 1.0f - ((float)(age_us - 80000) / 220000.0f);
        level *= fade;
    }
    return clamp_audio_level(level);
}

bool tater_audio_doa_snapshot(tater_audio_doa_t *out)
{
    if (!out) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    int64_t updated_us = 0;
    portENTER_CRITICAL(&s_doa_lock);
    *out = s_doa;
    updated_us = s_doa_update_us;
    portEXIT_CRITICAL(&s_doa_lock);
    if (updated_us <= 0 || now < updated_us) {
        out->age_ms = UINT32_MAX;
        return false;
    }
    int64_t age_ms = (now - updated_us) / 1000;
    out->age_ms = age_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)age_ms;
    return true;
}

bool tater_audio_xmos_status_snapshot(tater_audio_xmos_status_t *out)
{
    if (!out) {
        return false;
    }
    portENTER_CRITICAL(&s_xmos_status_lock);
    *out = s_xmos_status;
    portEXIT_CRITICAL(&s_xmos_status_lock);
    return true;
}

#endif
