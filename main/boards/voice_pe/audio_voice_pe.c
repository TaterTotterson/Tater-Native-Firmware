#include "audio_i2s.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tater_protocol.h"
#include "wake_engine.h"

static const char *TAG = "tater_audio";
static i2s_chan_handle_t s_rx_chan;
static i2s_chan_handle_t s_tx_chan;
static bool s_speaker_ready;
static bool s_speaker_enabled;
static bool s_speaker_preloaded;
static portMUX_TYPE s_speaker_level_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_speaker_audio_level;
static int64_t s_speaker_level_update_us;
static SemaphoreHandle_t s_i2c_mutex;
static portMUX_TYPE s_doa_lock = portMUX_INITIALIZER_UNLOCKED;
static tater_audio_doa_t s_doa;
static int64_t s_doa_update_us;
static uint32_t s_doa_failed_reads;
static portMUX_TYPE s_xmos_status_lock = portMUX_INITIALIZER_UNLOCKED;
static tater_audio_xmos_status_t s_xmos_status = {
    .target_major = 1,
    .target_minor = 3,
    .target_patch = 2,
    .update_state = TATER_XMOS_UPDATE_IDLE,
};

extern const uint8_t _binary_ffva_v1_3_2_vod_upgrade_bin_start[] asm("_binary_ffva_v1_3_2_vod_upgrade_bin_start");
extern const uint8_t _binary_ffva_v1_3_2_vod_upgrade_bin_end[] asm("_binary_ffva_v1_3_2_vod_upgrade_bin_end");

#define TATER_I2C_PORT I2C_NUM_0
#define AIC3204_I2C_ADDR 0x18
#define VOICE_KIT_I2C_ADDR 0x42

#define DFU_CONTROLLER_SERVICER_RESID 240
#define CONFIGURATION_SERVICER_RESID 241
#define DFU_COMMAND_READ_BIT 0x80
#define DFU_CONTROLLER_SERVICER_RESID_DFU_DNLOAD 1
#define DFU_CONTROLLER_SERVICER_RESID_DFU_GETSTATUS 3
#define DFU_CONTROLLER_SERVICER_RESID_DFU_SETALTERNATE 64
#define DFU_CONTROLLER_SERVICER_RESID_DFU_GETVERSION 88
#define DFU_CONTROLLER_SERVICER_RESID_DFU_REBOOT 89
#define DFU_INT_ALTERNATE_UPGRADE 1
#define DFU_INT_DFU_IDLE 2
#define DFU_INT_DFU_DNLOAD_IDLE 5
#define DFU_INT_DFU_MANIFEST_WAIT_RESET 8
#define DFU_INT_DFU_STATUS_OK 0
#define XMOS_DFU_MAX_XFER 128
#define XMOS_DFU_READY_TIMEOUT_MS 30000
#define XMOS_DFU_VERIFY_TIMEOUT_MS 10000
#define XMOS_VERSION_READY_TIMEOUT_MS 8000
#define XMOS_VERSION_RETRY_DELAY_MS 250
#define XMOS_TARGET_VERSION_MAJOR 1
#define XMOS_TARGET_VERSION_MINOR 3
#define XMOS_TARGET_VERSION_PATCH 2
#define CONFIGURATION_SERVICER_RESID_CHANNEL_0_PIPELINE_STAGE 0x30
#define CONFIGURATION_SERVICER_RESID_CHANNEL_1_PIPELINE_STAGE 0x40
#define PIPELINE_STAGE_NS 3
#define PIPELINE_STAGE_AGC 4
#define CTRL_DONE 0
#define DOA_RESOURCE_ID 231
#define DOA_CMD_READ_STATE 0x80
#define DOA_STATE_PAYLOAD_LEN 12
#define DOA_STATE_RESPONSE_LEN (DOA_STATE_PAYLOAD_LEN + 1)
#define DOA_FLAG_VALID (1u << 0)

#define AIC3204_PAGE_CTRL 0x00
#define AIC3204_SW_RST 0x01
#define AIC3204_NDAC 0x0B
#define AIC3204_MDAC 0x0C
#define AIC3204_DOSR 0x0E
#define AIC3204_CODEC_IF 0x1B
#define AIC3204_AUDIO_IF_4 0x1F
#define AIC3204_AUDIO_IF_5 0x20
#define AIC3204_SCLK_MFP3 0x38
#define AIC3204_DAC_SIG_PROC 0x3C
#define AIC3204_DAC_CH_SET1 0x3F
#define AIC3204_DAC_CH_SET2 0x40
#define AIC3204_DACL_VOL_D 0x41
#define AIC3204_DACR_VOL_D 0x42
#define AIC3204_PWR_CFG 0x01
#define AIC3204_LDO_CTRL 0x02
#define AIC3204_PLAY_CFG1 0x03
#define AIC3204_PLAY_CFG2 0x04
#define AIC3204_OP_PWR_CTRL 0x09
#define AIC3204_CM_CTRL 0x0A
#define AIC3204_HPL_ROUTE 0x0C
#define AIC3204_HPR_ROUTE 0x0D
#define AIC3204_LOL_ROUTE 0x0E
#define AIC3204_LOR_ROUTE 0x0F
#define AIC3204_HPL_GAIN 0x10
#define AIC3204_HPR_GAIN 0x11
#define AIC3204_LOL_DRV_GAIN 0x12
#define AIC3204_LOR_DRV_GAIN 0x13
#define AIC3204_HP_START 0x14
#define AIC3204_REF_STARTUP 0x7B
#define TATER_SPK_DMA_DESC_NUM 3
#define TATER_SPK_DMA_FRAME_NUM 360

static esp_err_t xmos_reset_boot(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TATER_XMOS_RESET,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "xmos reset gpio failed");
    gpio_set_level(TATER_XMOS_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(TATER_XMOS_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(3000));
    return ESP_OK;
}

static size_t xmos_target_firmware_size(void)
{
    return (size_t)(_binary_ffva_v1_3_2_vod_upgrade_bin_end - _binary_ffva_v1_3_2_vod_upgrade_bin_start);
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

static bool xmos_target_version_matches(uint8_t major, uint8_t minor, uint8_t patch)
{
    return major == XMOS_TARGET_VERSION_MAJOR
        && minor == XMOS_TARGET_VERSION_MINOR
        && patch == XMOS_TARGET_VERSION_PATCH;
}

static uint32_t read_u24_le(const uint8_t *payload)
{
    return (uint32_t)payload[0]
        | ((uint32_t)payload[1] << 8)
        | ((uint32_t)payload[2] << 16);
}

static esp_err_t i2c_init(void)
{
    if (!s_i2c_mutex) {
        s_i2c_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_i2c_mutex, ESP_ERR_NO_MEM, TAG, "i2c mutex alloc failed");
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TATER_I2C_SDA,
        .scl_io_num = TATER_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {
            .clk_speed = 400000,
        },
        .clk_flags = 0,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(TATER_I2C_PORT, &conf), TAG, "i2c config failed");
    esp_err_t err = i2c_driver_install(TATER_I2C_PORT, conf.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t locked_i2c_write(uint8_t addr, const uint8_t *data, size_t len, TickType_t timeout)
{
    if (s_i2c_mutex) {
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    }
    esp_err_t err = i2c_master_write_to_device(TATER_I2C_PORT, addr, data, len, timeout);
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
    return err;
}

static esp_err_t locked_i2c_read(uint8_t addr, uint8_t *data, size_t len, TickType_t timeout)
{
    if (s_i2c_mutex) {
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    }
    esp_err_t err = i2c_master_read_from_device(TATER_I2C_PORT, addr, data, len, timeout);
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
    return err;
}

static esp_err_t locked_i2c_write_read(
    uint8_t addr,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len,
    TickType_t timeout
)
{
    if (s_i2c_mutex) {
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    }
    esp_err_t err = i2c_master_write_read_device(
        TATER_I2C_PORT,
        addr,
        write_data,
        write_len,
        read_data,
        read_len,
        timeout
    );
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
    return err;
}

static esp_err_t aic_write(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return locked_i2c_write(AIC3204_I2C_ADDR, data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t aic_read(uint8_t reg, uint8_t *value)
{
    return locked_i2c_write_read(
        AIC3204_I2C_ADDR,
        &reg,
        1,
        value,
        1,
        pdMS_TO_TICKS(100)
    );
}

static esp_err_t aic_read_page_reg(uint8_t page, uint8_t reg, uint8_t *value)
{
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_PAGE_CTRL, page), TAG, "aic select page failed");
    return aic_read(reg, value);
}

static void aic3204_dump_state(const char *phase)
{
    uint8_t ndac = 0;
    uint8_t mdac = 0;
    uint8_t dosr = 0;
    uint8_t codec_if = 0;
    uint8_t dac_ch1 = 0;
    uint8_t dac_ch2 = 0;
    uint8_t vol_l = 0;
    uint8_t vol_r = 0;
    uint8_t op_pwr = 0;
    uint8_t hpl_route = 0;
    uint8_t hpr_route = 0;
    uint8_t lol_route = 0;
    uint8_t lor_route = 0;
    uint8_t hpl_gain = 0;
    uint8_t hpr_gain = 0;
    uint8_t lol_gain = 0;
    uint8_t lor_gain = 0;

    esp_err_t err = ESP_OK;
    err |= aic_read_page_reg(0x00, AIC3204_NDAC, &ndac);
    err |= aic_read_page_reg(0x00, AIC3204_MDAC, &mdac);
    err |= aic_read_page_reg(0x00, AIC3204_DOSR, &dosr);
    err |= aic_read_page_reg(0x00, AIC3204_CODEC_IF, &codec_if);
    err |= aic_read_page_reg(0x00, AIC3204_DAC_CH_SET1, &dac_ch1);
    err |= aic_read_page_reg(0x00, AIC3204_DAC_CH_SET2, &dac_ch2);
    err |= aic_read_page_reg(0x00, AIC3204_DACL_VOL_D, &vol_l);
    err |= aic_read_page_reg(0x00, AIC3204_DACR_VOL_D, &vol_r);
    err |= aic_read_page_reg(0x01, AIC3204_OP_PWR_CTRL, &op_pwr);
    err |= aic_read_page_reg(0x01, AIC3204_HPL_ROUTE, &hpl_route);
    err |= aic_read_page_reg(0x01, AIC3204_HPR_ROUTE, &hpr_route);
    err |= aic_read_page_reg(0x01, AIC3204_LOL_ROUTE, &lol_route);
    err |= aic_read_page_reg(0x01, AIC3204_LOR_ROUTE, &lor_route);
    err |= aic_read_page_reg(0x01, AIC3204_HPL_GAIN, &hpl_gain);
    err |= aic_read_page_reg(0x01, AIC3204_HPR_GAIN, &hpr_gain);
    err |= aic_read_page_reg(0x01, AIC3204_LOL_DRV_GAIN, &lol_gain);
    err |= aic_read_page_reg(0x01, AIC3204_LOR_DRV_GAIN, &lor_gain);
    ESP_ERROR_CHECK_WITHOUT_ABORT(aic_write(AIC3204_PAGE_CTRL, 0x00));

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "aic3204 state read failed phase=%s err=%s", phase, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(
        TAG,
        "aic3204 %s p0 ndac=0x%02x mdac=0x%02x dosr=0x%02x codec_if=0x%02x dac_ch1=0x%02x dac_ch2=0x%02x vol_l=0x%02x vol_r=0x%02x",
        phase,
        ndac,
        mdac,
        dosr,
        codec_if,
        dac_ch1,
        dac_ch2,
        vol_l,
        vol_r
    );
    ESP_LOGI(
        TAG,
        "aic3204 %s p1 op_pwr=0x%02x routes hpl=0x%02x hpr=0x%02x lol=0x%02x lor=0x%02x gains hpl=0x%02x hpr=0x%02x lol=0x%02x lor=0x%02x",
        phase,
        op_pwr,
        hpl_route,
        hpr_route,
        lol_route,
        lor_route,
        hpl_gain,
        hpr_gain,
        lol_gain,
        lor_gain
    );
}

static esp_err_t voice_kit_read_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
    const uint8_t req[] = {
        DFU_CONTROLLER_SERVICER_RESID,
        DFU_CONTROLLER_SERVICER_RESID_DFU_GETVERSION | DFU_COMMAND_READ_BIT,
        4,
    };
    uint8_t resp[4] = {0};
    esp_err_t err = locked_i2c_write(VOICE_KIT_I2C_ADDR, req, sizeof(req), pdMS_TO_TICKS(250));
    if (err != ESP_OK) {
        return err;
    }
    err = locked_i2c_read(VOICE_KIT_I2C_ADDR, resp, sizeof(resp), pdMS_TO_TICKS(250));
    if (err != ESP_OK) {
        return err;
    }
    if (resp[0] != CTRL_DONE) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "xmos firmware version %u.%u.%u", resp[1], resp[2], resp[3]);
    if (major) {
        *major = resp[1];
    }
    if (minor) {
        *minor = resp[2];
    }
    if (patch) {
        *patch = resp[3];
    }
    xmos_status_set_version(true, resp[1], resp[2], resp[3]);
    return ESP_OK;
}

static esp_err_t xmos_read_version_with_retry(
    uint8_t *major,
    uint8_t *minor,
    uint8_t *patch,
    uint32_t timeout_ms,
    const char *phase
)
{
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    uint32_t attempts = 0;
    esp_err_t last_err = ESP_FAIL;
    while (esp_timer_get_time() <= deadline_us) {
        attempts++;
        last_err = voice_kit_read_version(major, minor, patch);
        if (last_err == ESP_OK) {
            if (attempts > 1) {
                ESP_LOGI(TAG, "xmos version ready after %" PRIu32 " attempt(s)", attempts);
            }
            return ESP_OK;
        }
        if (attempts == 1 || (attempts % 10) == 0) {
            ESP_LOGW(
                TAG,
                "xmos version not ready during %s attempt=%" PRIu32 " err=%s",
                phase ? phase : "startup",
                attempts,
                esp_err_to_name(last_err)
            );
        }
        vTaskDelay(pdMS_TO_TICKS(XMOS_VERSION_RETRY_DELAY_MS));
    }

    ESP_LOGE(
        TAG,
        "xmos version unavailable after %" PRIu32 " ms during %s err=%s",
        timeout_ms,
        phase ? phase : "startup",
        esp_err_to_name(last_err)
    );
    xmos_status_set_version(false, 0, 0, 0);
    return last_err == ESP_OK ? ESP_ERR_TIMEOUT : last_err;
}

static esp_err_t xmos_dfu_get_status(uint8_t *dfu_state, uint8_t *dfu_status, uint32_t *next_delay_ms)
{
    const uint8_t req[] = {
        DFU_CONTROLLER_SERVICER_RESID,
        DFU_CONTROLLER_SERVICER_RESID_DFU_GETSTATUS | DFU_COMMAND_READ_BIT,
        6,
    };
    uint8_t resp[6] = {0};

    ESP_RETURN_ON_ERROR(
        locked_i2c_write(VOICE_KIT_I2C_ADDR, req, sizeof(req), pdMS_TO_TICKS(100)),
        TAG,
        "xmos dfu status request failed"
    );
    ESP_RETURN_ON_ERROR(
        locked_i2c_read(VOICE_KIT_I2C_ADDR, resp, sizeof(resp), pdMS_TO_TICKS(100)),
        TAG,
        "xmos dfu status read failed"
    );
    ESP_RETURN_ON_FALSE(resp[0] == CTRL_DONE, ESP_FAIL, TAG, "xmos dfu status response not ready");

    uint8_t status = resp[1];
    uint8_t state = resp[5];
    uint32_t delay_ms = read_u24_le(&resp[2]);
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
    if (status != DFU_INT_DFU_STATUS_OK) {
        ESP_LOGW(TAG, "xmos dfu status non-ok status=%u state=%u delay_ms=%" PRIu32, status, state, delay_ms);
    }
    return status == DFU_INT_DFU_STATUS_OK ? ESP_OK : ESP_FAIL;
}

static bool xmos_dfu_state_ready(uint8_t state)
{
    return state == DFU_INT_DFU_IDLE
        || state == DFU_INT_DFU_DNLOAD_IDLE
        || state == DFU_INT_DFU_MANIFEST_WAIT_RESET;
}

static esp_err_t xmos_dfu_wait_ready(const char *phase, uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);
    while (esp_timer_get_time() <= deadline_us) {
        uint8_t state = 0;
        uint8_t status = 0;
        uint32_t delay_ms = 0;
        esp_err_t err = xmos_dfu_get_status(&state, &status, &delay_ms);
        if (err == ESP_OK && xmos_dfu_state_ready(state)) {
            return ESP_OK;
        }

        if (delay_ms == 0) {
            delay_ms = 20;
        } else if (delay_ms > 250) {
            delay_ms = 250;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    ESP_LOGE(TAG, "xmos dfu timed out waiting for %s", phase ? phase : "ready");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t xmos_dfu_set_alternate(void)
{
    const uint8_t req[] = {
        DFU_CONTROLLER_SERVICER_RESID,
        DFU_CONTROLLER_SERVICER_RESID_DFU_SETALTERNATE,
        1,
        DFU_INT_ALTERNATE_UPGRADE,
    };
    return locked_i2c_write(VOICE_KIT_I2C_ADDR, req, sizeof(req), pdMS_TO_TICKS(100));
}

static esp_err_t xmos_dfu_reboot(void)
{
    const uint8_t req[] = {
        DFU_CONTROLLER_SERVICER_RESID,
        DFU_CONTROLLER_SERVICER_RESID_DFU_REBOOT,
        1,
        0,
    };
    return locked_i2c_write(VOICE_KIT_I2C_ADDR, req, sizeof(req), pdMS_TO_TICKS(100));
}

static esp_err_t xmos_dfu_send_download_block(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(len <= XMOS_DFU_MAX_XFER, ESP_ERR_INVALID_SIZE, TAG, "xmos dfu block too large");
    uint8_t req[XMOS_DFU_MAX_XFER + 5] = {
        DFU_CONTROLLER_SERVICER_RESID,
        DFU_CONTROLLER_SERVICER_RESID_DFU_DNLOAD,
        XMOS_DFU_MAX_XFER + 2,
        (uint8_t)len,
        0,
    };
    if (data && len > 0) {
        memcpy(&req[5], data, len);
    }
    return locked_i2c_write(VOICE_KIT_I2C_ADDR, req, sizeof(req), pdMS_TO_TICKS(250));
}

static esp_err_t xmos_wait_for_target_version(void)
{
    uint8_t major = 0;
    uint8_t minor = 0;
    uint8_t patch = 0;
    esp_err_t err = xmos_read_version_with_retry(&major, &minor, &patch, XMOS_DFU_VERIFY_TIMEOUT_MS, "dfu verify");
    if (err == ESP_OK && xmos_target_version_matches(major, minor, patch)) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t xmos_dfu_update_to_target(void)
{
    const uint8_t *image = _binary_ffva_v1_3_2_vod_upgrade_bin_start;
    size_t image_size = xmos_target_firmware_size();
    ESP_RETURN_ON_FALSE(image && image_size > 0, ESP_ERR_INVALID_SIZE, TAG, "xmos target firmware image missing");

    xmos_status_set_update_flags(true, true);
    xmos_status_set_update_state(TATER_XMOS_UPDATE_RUNNING);
    xmos_status_set_progress(0);
    ESP_LOGW(
        TAG,
        "xmos firmware update starting target=%u.%u.%u size=%u",
        XMOS_TARGET_VERSION_MAJOR,
        XMOS_TARGET_VERSION_MINOR,
        XMOS_TARGET_VERSION_PATCH,
        (unsigned)image_size
    );

    ESP_RETURN_ON_ERROR(xmos_dfu_set_alternate(), TAG, "xmos dfu set alternate failed");

    size_t offset = 0;
    uint8_t last_progress = 0;
    while (offset < image_size) {
        ESP_RETURN_ON_ERROR(xmos_dfu_wait_ready("download", XMOS_DFU_READY_TIMEOUT_MS), TAG, "xmos dfu download wait failed");
        size_t remaining = image_size - offset;
        size_t chunk = remaining > XMOS_DFU_MAX_XFER ? XMOS_DFU_MAX_XFER : remaining;
        ESP_RETURN_ON_ERROR(xmos_dfu_send_download_block(image + offset, chunk), TAG, "xmos dfu download block failed");
        offset += chunk;

        uint8_t progress = (uint8_t)((offset * 100U) / image_size);
        if (progress != last_progress && (progress == 100 || progress >= (uint8_t)(last_progress + 5))) {
            last_progress = progress;
            xmos_status_set_progress(progress);
            ESP_LOGI(TAG, "xmos firmware update progress=%u%%", progress);
        }
    }

    ESP_RETURN_ON_ERROR(xmos_dfu_wait_ready("finalize", XMOS_DFU_READY_TIMEOUT_MS), TAG, "xmos dfu finalize wait failed");
    ESP_RETURN_ON_ERROR(xmos_dfu_send_download_block(NULL, 0), TAG, "xmos dfu final download failed");
    ESP_RETURN_ON_ERROR(xmos_dfu_wait_ready("manifest", XMOS_DFU_READY_TIMEOUT_MS), TAG, "xmos dfu manifest wait failed");
    ESP_LOGI(TAG, "xmos firmware image written; rebooting XMOS");
    ESP_RETURN_ON_ERROR(xmos_dfu_reboot(), TAG, "xmos dfu reboot failed");
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_RETURN_ON_ERROR(xmos_wait_for_target_version(), TAG, "xmos dfu version verify failed");

    xmos_status_set_progress(100);
    xmos_status_set_update_state(TATER_XMOS_UPDATE_COMPLETE);
    ESP_LOGI(TAG, "xmos firmware update complete");
    return ESP_OK;
}

static esp_err_t xmos_ensure_target_firmware(void)
{
    uint8_t major = 0;
    uint8_t minor = 0;
    uint8_t patch = 0;
    esp_err_t err = xmos_read_version_with_retry(&major, &minor, &patch, XMOS_VERSION_READY_TIMEOUT_MS, "startup");
    if (err != ESP_OK) {
        xmos_status_set_update_state(TATER_XMOS_UPDATE_ERROR);
        return err;
    }

    if (xmos_target_version_matches(major, minor, patch)) {
        xmos_status_set_update_flags(false, false);
        xmos_status_set_progress(100);
        xmos_status_set_update_state(TATER_XMOS_UPDATE_SKIPPED);
        ESP_LOGI(TAG, "xmos firmware target already installed");
        return ESP_OK;
    }

    ESP_LOGW(
        TAG,
        "xmos firmware mismatch installed=%u.%u.%u target=%u.%u.%u",
        major,
        minor,
        patch,
        XMOS_TARGET_VERSION_MAJOR,
        XMOS_TARGET_VERSION_MINOR,
        XMOS_TARGET_VERSION_PATCH
    );
    err = xmos_dfu_update_to_target();
    if (err != ESP_OK) {
        xmos_status_set_update_state(TATER_XMOS_UPDATE_ERROR);
    }
    return err;
}

static esp_err_t voice_kit_write_stage(uint8_t reg, uint8_t stage)
{
    uint8_t req[] = {CONFIGURATION_SERVICER_RESID, reg, 1, stage};
    return locked_i2c_write(VOICE_KIT_I2C_ADDR, req, sizeof(req), pdMS_TO_TICKS(100));
}

static esp_err_t voice_kit_configure_pipeline(void)
{
    ESP_RETURN_ON_ERROR(xmos_ensure_target_firmware(), TAG, "xmos target firmware check failed");
    ESP_RETURN_ON_ERROR(
        voice_kit_write_stage(CONFIGURATION_SERVICER_RESID_CHANNEL_0_PIPELINE_STAGE, PIPELINE_STAGE_AGC),
        TAG,
        "xmos channel 0 stage failed"
    );
    ESP_RETURN_ON_ERROR(
        voice_kit_write_stage(CONFIGURATION_SERVICER_RESID_CHANNEL_1_PIPELINE_STAGE, PIPELINE_STAGE_NS),
        TAG,
        "xmos channel 1 stage failed"
    );
    ESP_LOGI(TAG, "xmos pipeline configured channel0=AGC channel1=NS");
    return ESP_OK;
}

static uint32_t read_u32_le(const uint8_t *payload)
{
    return (uint32_t)payload[0]
        | ((uint32_t)payload[1] << 8)
        | ((uint32_t)payload[2] << 16)
        | ((uint32_t)payload[3] << 24);
}

static esp_err_t xmos_read_doa(tater_audio_doa_t *out)
{
    const uint8_t req[] = {DOA_RESOURCE_ID, DOA_CMD_READ_STATE, DOA_STATE_RESPONSE_LEN};
    uint8_t resp[DOA_STATE_RESPONSE_LEN] = {0};

    ESP_RETURN_ON_ERROR(
        locked_i2c_write(VOICE_KIT_I2C_ADDR, req, sizeof(req), pdMS_TO_TICKS(100)),
        TAG,
        "xmos doa request failed"
    );
    ESP_RETURN_ON_ERROR(
        locked_i2c_read(VOICE_KIT_I2C_ADDR, resp, sizeof(resp), pdMS_TO_TICKS(100)),
        TAG,
        "xmos doa read failed"
    );
    ESP_RETURN_ON_FALSE(resp[0] == CTRL_DONE, ESP_FAIL, TAG, "xmos doa response not ready");

    const uint8_t *payload = &resp[1];
    out->sample_delay = (int16_t)((uint16_t)payload[0] | ((uint16_t)payload[1] << 8));
    out->confidence = payload[2];
    out->valid = (payload[3] & DOA_FLAG_VALID) != 0;
    out->energy = read_u32_le(&payload[4]);
    out->frame_counter = read_u32_le(&payload[8]);
    out->age_ms = 0;
    return ESP_OK;
}

static void store_doa_snapshot(const tater_audio_doa_t *snapshot)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_doa_lock);
    s_doa = *snapshot;
    s_doa_update_us = now;
    portEXIT_CRITICAL(&s_doa_lock);
}

static void doa_task(void *arg)
{
    (void)arg;
    while (true) {
        tater_audio_doa_t snapshot = {0};
        esp_err_t err = xmos_read_doa(&snapshot);
        if (err == ESP_OK) {
            if (s_doa_failed_reads > 0) {
                ESP_LOGI(TAG, "xmos doa recovered");
            }
            s_doa_failed_reads = 0;
            store_doa_snapshot(&snapshot);
        } else {
            s_doa_failed_reads++;
            if (s_doa_failed_reads == 1 || (s_doa_failed_reads % 50) == 0) {
                ESP_LOGW(TAG, "xmos doa read failed count=%lu err=%s", (unsigned long)s_doa_failed_reads, esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t aic3204_apply_clock_settings(void)
{
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_PAGE_CTRL, 0x00), TAG, "aic page 0 failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_NDAC, 0x82), TAG, "aic ndac failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_MDAC, 0x82), TAG, "aic mdac failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_DOSR, 0x80), TAG, "aic dosr failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_CODEC_IF, 0x30), TAG, "aic iface failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_SCLK_MFP3, 0x02), TAG, "aic mfp3 failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_AUDIO_IF_4, 0x01), TAG, "aic iface4 failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_AUDIO_IF_5, 0x01), TAG, "aic iface5 failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_DAC_SIG_PROC, 0x01), TAG, "aic dac proc failed");
    return ESP_OK;
}

static esp_err_t aic3204_apply_output_settings(void)
{
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_PAGE_CTRL, 0x01), TAG, "aic page 1 failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_LDO_CTRL, 0x09), TAG, "aic ldo failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_PWR_CFG, 0x08), TAG, "aic power cfg failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_LDO_CTRL, 0x01), TAG, "aic analog power failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_CM_CTRL, 0x40), TAG, "aic cm failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_PLAY_CFG1, 0x00), TAG, "aic play1 failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_PLAY_CFG2, 0x00), TAG, "aic play2 failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_REF_STARTUP, 0x01), TAG, "aic ref failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_HP_START, 0x25), TAG, "aic hp start failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_HPL_ROUTE, 0x08), TAG, "aic hpl route failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_HPR_ROUTE, 0x08), TAG, "aic hpr route failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_LOL_ROUTE, 0x08), TAG, "aic lol route failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_LOR_ROUTE, 0x08), TAG, "aic lor route failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_HPL_GAIN, 0x3e), TAG, "aic hpl gain failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_HPR_GAIN, 0x3e), TAG, "aic hpr gain failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_LOL_DRV_GAIN, 0x00), TAG, "aic lol gain failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_LOR_DRV_GAIN, 0x00), TAG, "aic lor gain failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_OP_PWR_CTRL, 0x3C), TAG, "aic output power failed");
    return ESP_OK;
}

static esp_err_t aic3204_apply_dac_settings(void)
{
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_PAGE_CTRL, 0x00), TAG, "aic page 0 failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_DAC_CH_SET1, 0xd4), TAG, "aic dac power failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_DACL_VOL_D, 0x10), TAG, "aic left volume failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_DACR_VOL_D, 0x10), TAG, "aic right volume failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_DAC_CH_SET2, 0x00), TAG, "aic unmute failed");
    return ESP_OK;
}

static esp_err_t aic3204_apply_playback_settings(void)
{
    ESP_RETURN_ON_ERROR(aic3204_apply_clock_settings(), TAG, "aic clock settings failed");
    ESP_RETURN_ON_ERROR(aic3204_apply_output_settings(), TAG, "aic output settings failed");
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_RETURN_ON_ERROR(aic3204_apply_dac_settings(), TAG, "aic dac settings failed");
    return ESP_OK;
}

static esp_err_t aic3204_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c init failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_PAGE_CTRL, 0x00), TAG, "aic page 0 failed");
    ESP_RETURN_ON_ERROR(aic_write(AIC3204_SW_RST, 0x01), TAG, "aic reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(aic3204_apply_clock_settings(), TAG, "aic clock settings failed");
    ESP_RETURN_ON_ERROR(aic3204_apply_output_settings(), TAG, "aic output settings failed");
    vTaskDelay(pdMS_TO_TICKS(2500));
    ESP_RETURN_ON_ERROR(aic3204_apply_dac_settings(), TAG, "aic dac settings failed");
    aic3204_dump_state("after-init");
    ESP_LOGI(TAG, "aic3204 ready");
    return ESP_OK;
}

static esp_err_t speaker_i2s_configure_channel(void)
{
    if (s_tx_chan) {
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "speaker i2s configure dma_free=%u dma_largest=%u desc=%u frames=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        (unsigned)TATER_SPK_DMA_DESC_NUM,
        (unsigned)TATER_SPK_DMA_FRAME_NUM
    );
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_SLAVE,
        .dma_desc_num = TATER_SPK_DMA_DESC_NUM,
        .dma_frame_num = TATER_SPK_DMA_FRAME_NUM,
        .auto_clear = true,
        .intr_priority = 3,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL), TAG, "speaker i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = TATER_SPK_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = TATER_SPK_I2S_BCLK,
            .ws = TATER_SPK_I2S_WS,
            .dout = TATER_SPK_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    esp_err_t err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return err;
    }
    return ESP_OK;
}

static esp_err_t speaker_i2s_init(void)
{
    esp_err_t err = speaker_i2s_configure_channel();
    if (err != ESP_OK) {
        s_speaker_ready = false;
        s_speaker_enabled = false;
        s_speaker_preloaded = false;
        return err;
    }
    s_speaker_ready = true;
    s_speaker_enabled = false;
    s_speaker_preloaded = false;
    ESP_LOGI(TAG, "speaker i2s ready bclk=%d ws=%d dout=%d", TATER_SPK_I2S_BCLK, TATER_SPK_I2S_WS, TATER_SPK_I2S_DOUT);
    return ESP_OK;
}

static esp_err_t speaker_i2s_start_driver(void)
{
    ESP_RETURN_ON_ERROR(speaker_i2s_configure_channel(), TAG, "speaker i2s configure failed");
    if (s_speaker_enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_chan), TAG, "speaker i2s disable failed");
        s_speaker_enabled = false;
    }

    s_speaker_preloaded = false;
    return ESP_OK;
}

esp_err_t tater_audio_i2s_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c init failed");
    ESP_ERROR_CHECK_WITHOUT_ABORT(xmos_reset_boot());
    ESP_ERROR_CHECK_WITHOUT_ABORT(voice_kit_configure_pipeline());
    ESP_ERROR_CHECK_WITHOUT_ABORT(aic3204_init());

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = TATER_MIC_CHUNK_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan), TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TATER_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = TATER_MIC_I2S_BCLK,
            .ws = TATER_MIC_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = TATER_MIC_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "i2s init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "i2s enable failed");
    ESP_LOGI(TAG, "mic i2s ready bclk=%d ws=%d din=%d", TATER_MIC_I2S_BCLK, TATER_MIC_I2S_WS, TATER_MIC_I2S_DIN);
    ESP_ERROR_CHECK_WITHOUT_ABORT(speaker_i2s_init());
    return ESP_OK;
}

static int16_t pcm32_to_pcm16(int32_t sample)
{
    int32_t v = sample >> 16;
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)v;
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
    int32_t rx[TATER_MIC_CHUNK_FRAMES * TATER_MIC_SOURCE_CHANNELS];
    int16_t mono[TATER_MIC_CHUNK_FRAMES];
    uint32_t active_read_errors = 0;
    uint32_t active_chunks = 0;
    bool last_active = false;

    while (true) {
        bool active = tater_protocol_voice_active() && tater_protocol_is_connected();
        if (active && !last_active) {
            active_read_errors = 0;
            active_chunks = 0;
            ESP_LOGI(TAG, "mic stream active; waiting for i2s frames");
        }
        last_active = active;

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, rx, sizeof(rx), &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) {
            if (active && (++active_read_errors % 10) == 0) {
                ESP_LOGW(TAG, "mic i2s read waiting err=%s bytes=%u", esp_err_to_name(err), (unsigned)bytes_read);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t frames = bytes_read / (sizeof(int32_t) * TATER_MIC_SOURCE_CHANNELS);
        if (frames > TATER_MIC_CHUNK_FRAMES) {
            frames = TATER_MIC_CHUNK_FRAMES;
        }

        for (size_t i = 0; i < frames; i++) {
            int32_t left = rx[i * 2];
            int32_t right = rx[(i * 2) + 1];
            int32_t mixed = ((int32_t)pcm32_to_pcm16(left) + (int32_t)pcm32_to_pcm16(right)) / 2;
            mono[i] = (int16_t)mixed;
        }

        tater_wake_engine_note_audio(mono, frames);
        tater_wake_engine_process(mono, frames);

        if (active) {
            if (active_chunks < 3) {
                ESP_LOGI(TAG, "mic chunk %u frames=%u bytes=%u", (unsigned)(active_chunks + 1), (unsigned)frames, (unsigned)bytes_read);
            }
            active_chunks++;
            tater_protocol_send_audio(mono, frames);
        }
    }
}

void tater_audio_i2s_start_task(void)
{
    xTaskCreatePinnedToCore(audio_task, "tater_audio", 8192, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(doa_task, "tater_doa", 4096, NULL, 4, NULL, 0);
}

esp_err_t tater_audio_speaker_begin(void)
{
    reset_speaker_audio_level();
    ESP_RETURN_ON_ERROR(aic3204_apply_playback_settings(), TAG, "speaker dac settings failed");
    aic3204_dump_state("before-speaker-start");
    ESP_RETURN_ON_ERROR(speaker_i2s_start_driver(), TAG, "speaker i2s start failed");
    return ESP_OK;
}

esp_err_t tater_audio_write_speaker_frames(const int16_t *stereo_frames, size_t frame_count)
{
    if (!s_tx_chan || !stereo_frames || frame_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    update_speaker_audio_level(stereo_frames, frame_count);

    const uint8_t *data = (const uint8_t *)stereo_frames;
    size_t byte_count = frame_count * TATER_SPK_CHANNELS * sizeof(int16_t);
    size_t bytes_written = 0;
    if (!s_speaker_preloaded) {
        if (s_speaker_enabled) {
            ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_chan), TAG, "speaker i2s disable failed");
            s_speaker_enabled = false;
        }
        ESP_RETURN_ON_ERROR(
            i2s_channel_preload_data(s_tx_chan, data, byte_count, &bytes_written),
            TAG,
            "speaker i2s preload failed"
        );
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "speaker i2s enable failed");
        s_speaker_enabled = true;
        s_speaker_preloaded = true;
        if (bytes_written >= byte_count) {
            return ESP_OK;
        }
        data += bytes_written;
        byte_count -= bytes_written;
    }
    return i2s_channel_write(
        s_tx_chan,
        data,
        byte_count,
        &bytes_written,
        pdMS_TO_TICKS(1000)
    );
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
    s_speaker_preloaded = false;
    reset_speaker_audio_level();
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
