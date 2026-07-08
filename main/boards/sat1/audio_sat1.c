#include "audio_i2s.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio_aec.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tater_protocol.h"
#include "wake_engine.h"

#if TATER_BOARD_SAT1

static const char *TAG = "tater_audio_sat1";

#define TATER_I2C_PORT I2C_NUM_0
#define SAT1_SPI_HOST SPI2_HOST
#define SAT1_CONTROL_CMD_READ_BIT 0x80
#define SAT1_CONTROL_COMMAND_IGNORED 7
#define SAT1_STATUS_REGISTER_LEN 4
#define SAT1_DFU_CONTROLLER_RESID 240
#define SAT1_DFU_GET_VERSION (88 | SAT1_CONTROL_CMD_READ_BIT)
#define SAT1_DOA_RESOURCE_ID 231
#define SAT1_DOA_READ_STATE SAT1_CONTROL_CMD_READ_BIT
#define SAT1_DOA_STATE_PAYLOAD_LEN 32
#define SAT1_DOA_FLAG_VALID (1u << 0)
#define SAT1_DOA_FLAG_FOUR_MIC (1u << 1)
#define SAT1_XMOS_TARGET_MAJOR 1
#define SAT1_XMOS_TARGET_MINOR 0
#define SAT1_XMOS_TARGET_PATCH 4
#define SAT1_MIC_GAIN_Q8 2048
#define SAT1_MIC_SAMPLE_PHASE 0
#define SAT1_SPK_DMA_DESC_NUM 4
#define SAT1_SPK_DMA_FRAME_NUM 240
#define SAT1_SPK_WRITE_FRAMES SAT1_SPK_DMA_FRAME_NUM

static i2s_chan_handle_t s_rx_chan;
static i2s_chan_handle_t s_tx_chan;
static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_i2c_mutex;
static SemaphoreHandle_t s_spi_mutex;
static bool s_speaker_ready;
static bool s_speaker_enabled;
static bool s_speaker_primed;
static uint8_t s_tas_power_mode = 0xff;
static portMUX_TYPE s_speaker_level_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_speaker_audio_level;
static int64_t s_speaker_level_update_us;
static portMUX_TYPE s_doa_lock = portMUX_INITIALIZER_UNLOCKED;
static tater_audio_doa_t s_doa;
static int64_t s_doa_update_us;
static uint32_t s_doa_failed_reads;
static int64_t s_doa_last_log_us;
static bool s_doa_have_frame_counter;
static uint32_t s_doa_last_frame_counter;
static int64_t s_doa_last_frame_change_us;
static portMUX_TYPE s_xmos_status_lock = portMUX_INITIALIZER_UNLOCKED;
static tater_audio_xmos_status_t s_xmos_status = {
    .target_major = SAT1_XMOS_TARGET_MAJOR,
    .target_minor = SAT1_XMOS_TARGET_MINOR,
    .target_patch = SAT1_XMOS_TARGET_PATCH,
    .update_state = TATER_XMOS_UPDATE_IDLE,
};

static uint32_t read_u32_le(const uint8_t *payload)
{
    return (uint32_t)payload[0]
        | ((uint32_t)payload[1] << 8)
        | ((uint32_t)payload[2] << 16)
        | ((uint32_t)payload[3] << 24);
}

static int16_t read_i16_le(const uint8_t *payload)
{
    return (int16_t)((uint16_t)payload[0] | ((uint16_t)payload[1] << 8));
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

static esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    if (s_i2c_mutex) {
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    }
    esp_err_t err = i2c_master_write_to_device(TATER_I2C_PORT, addr, data, sizeof(data), pdMS_TO_TICKS(100));
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
    return err;
}

static esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_i2c_mutex) {
        xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    }
    esp_err_t err = i2c_master_write_read_device(
        TATER_I2C_PORT,
        addr,
        &reg,
        1,
        value,
        1,
        pdMS_TO_TICKS(100)
    );
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
    return err;
}

static esp_err_t pcm5122_set_mute(bool muted)
{
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x00, 0x00), TAG, "pcm page failed");
    return i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x03, muted ? 0x11 : 0x00);
}

static esp_err_t pcm5122_set_volume(float volume)
{
    if (volume < 0.0f) {
        volume = 0.0f;
    } else if (volume > 1.0f) {
        volume = 1.0f;
    }
    const uint8_t min_byte = 0x44;
    const uint8_t max_byte = 0x99;
    uint8_t volume_byte = (uint8_t)(min_byte + ((1.0f - volume) * (float)(max_byte - min_byte) + 0.5f));
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x00, 0x00), TAG, "pcm page failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x3D, volume_byte), TAG, "pcm left volume failed");
    return i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x3E, volume_byte);
}

static esp_err_t pcm5122_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x00, 0x00), TAG, "pcm page failed");
    uint8_t chd1 = 0;
    uint8_t chd2 = 0;
    esp_err_t r1 = i2c_read_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x09, &chd1);
    esp_err_t r2 = i2c_read_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x10, &chd2);
    if (r1 != ESP_OK || r2 != ESP_OK || chd1 != 0x00 || chd2 != 0x00) {
        ESP_LOGW(TAG, "pcm5122 not confirmed r1=%s r2=%s chd1=0x%02x chd2=0x%02x", esp_err_to_name(r1), esp_err_to_name(r2), chd1, chd2);
    }

    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x01, 0x10), TAG, "pcm reset assert failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x01, 0x00), TAG, "pcm reset release failed");
    uint8_t err_detect = 0;
    if (i2c_read_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x25, &err_detect) == ESP_OK) {
        err_detect |= (1 << 3);
        err_detect &= ~(1 << 1);
        ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x25, err_detect), TAG, "pcm err detect failed");
    }
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x28, 0x03), TAG, "pcm 32-bit i2s failed");
    uint8_t pll_ref = 0;
    if (i2c_read_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x0D, &pll_ref) == ESP_OK) {
        pll_ref &= ~(7 << 4);
        pll_ref |= (1 << 4);
        ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_PCM5122_I2C_ADDR, 0x0D, pll_ref), TAG, "pcm pll ref failed");
    }
    ESP_RETURN_ON_ERROR(pcm5122_set_volume(0.75f), TAG, "pcm volume failed");
    ESP_RETURN_ON_ERROR(pcm5122_set_mute(true), TAG, "pcm mute failed");
    ESP_LOGI(TAG, "pcm5122 ready");
    return ESP_OK;
}

static esp_err_t tas2780_write_page(uint8_t page)
{
    return i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x00, page);
}

static esp_err_t tas2780_set_volume(float volume)
{
    if (volume < 0.0f) {
        volume = 0.0f;
    } else if (volume > 1.0f) {
        volume = 1.0f;
    }
    const float min_volume = 0.30f;
    const float max_volume = 1.0f;
    float scaled_volume = (volume * (max_volume - min_volume)) + min_volume;
    uint8_t attenuation = (uint8_t)((1.0f - scaled_volume) * 100.0f);
    if (attenuation > 0xC8) {
        attenuation = 0xC8;
    }
    ESP_RETURN_ON_ERROR(tas2780_write_page(0x00), TAG, "tas page failed");
    return i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x1A, attenuation);
}

static esp_err_t tas2780_set_mute(bool muted)
{
    if (muted) {
        ESP_RETURN_ON_ERROR(tas2780_write_page(0x00), TAG, "tas page failed");
        return i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x1A, 0xC9);
    }
    return tas2780_set_volume(0.8f);
}

static esp_err_t tas2780_set_power_mode(uint8_t power_mode)
{
    static const uint8_t power_modes[4][2] = {
        {2, 0},
        {0, 0},
        {3, 1},
        {1, 0},
    };
    if (power_mode > 3) {
        power_mode = 0;
    }
    ESP_RETURN_ON_ERROR(tas2780_write_page(0x00), TAG, "tas page failed");
    uint8_t chnl_0 = 0;
    uint8_t dc_blk0 = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x03, &chnl_0), TAG, "tas chnl read failed");
    ESP_RETURN_ON_ERROR(i2c_read_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x04, &dc_blk0), TAG, "tas dc read failed");
    chnl_0 = (chnl_0 & ~(0x03 << 6)) | (power_modes[power_mode][0] << 6);
    dc_blk0 = (dc_blk0 & ~(1 << 7)) | (power_modes[power_mode][1] << 7);
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x03, chnl_0), TAG, "tas chnl write failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x04, dc_blk0), TAG, "tas dc write failed");
    s_tas_power_mode = power_mode;
    return ESP_OK;
}

static esp_err_t tas2780_update_register(void)
{
    ESP_RETURN_ON_ERROR(tas2780_write_page(0x00), TAG, "tas page failed");
    uint8_t chnl_0 = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x03, &chnl_0), TAG, "tas amp read failed");
    chnl_0 &= ~(0x1F << 1);
    chnl_0 |= (8 << 1);
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x03, chnl_0), TAG, "tas amp write failed");
    return i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x0A, (3 << 4) | (3 << 2) | 2);
}

static esp_err_t tas2780_init(uint8_t power_mode)
{
    ESP_RETURN_ON_ERROR(tas2780_write_page(0x00), TAG, "tas page failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x01, 0x01), TAG, "tas reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t chip = 0;
    esp_err_t chip_err = i2c_read_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x05, &chip);
    if (chip_err != ESP_OK || chip != 0x41) {
        ESP_LOGW(TAG, "tas2780 not confirmed err=%s chip=0x%02x", esp_err_to_name(chip_err), chip);
    }

    ESP_RETURN_ON_ERROR(tas2780_write_page(0x00), TAG, "tas page failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x0E, 0x44), TAG, "tas cfg5 failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x0F, 0x40), TAG, "tas cfg6 failed");
    ESP_RETURN_ON_ERROR(tas2780_write_page(0x01), TAG, "tas page1 failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x19, 0x00), TAG, "tas lsr failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x17, 0xC8), TAG, "tas init0 failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x21, 0x00), TAG, "tas init1 failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x35, 0x74), TAG, "tas init2 failed");
    ESP_RETURN_ON_ERROR(tas2780_write_page(0xFD), TAG, "tas pagefd failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x0D, 0x0D), TAG, "tas fd access failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x3E, 0x4A), TAG, "tas dmin failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x0D, 0x00), TAG, "tas fd release failed");
    ESP_RETURN_ON_ERROR(tas2780_set_power_mode(power_mode), TAG, "tas power mode failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x71, 0x03), TAG, "tas uvlo failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x3D, 0xFF), TAG, "tas mask4 failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x40, 0xFF), TAG, "tas mask2 failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x41, 0xFF), TAG, "tas mask3 failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x3C, 0xFF), TAG, "tas mask1 failed");
    ESP_RETURN_ON_ERROR(tas2780_update_register(), TAG, "tas update register failed");
    ESP_RETURN_ON_ERROR(tas2780_set_mute(true), TAG, "tas mute failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x02, 0x82), TAG, "tas shutdown failed");
    ESP_LOGI(TAG, "tas2780 ready power_mode=%u", power_mode);
    return ESP_OK;
}

static esp_err_t tas2780_activate(uint8_t power_mode)
{
    ESP_RETURN_ON_ERROR(tas2780_write_page(0x00), TAG, "tas page failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x5C, 0x1D), TAG, "tas int clear failed");
    if (s_tas_power_mode != power_mode) {
        ESP_RETURN_ON_ERROR(tas2780_init(power_mode), TAG, "tas reinit failed");
    }
    ESP_RETURN_ON_ERROR(i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x02, 0x80), TAG, "tas activate failed");
    return tas2780_set_mute(false);
}

static esp_err_t tas2780_deactivate(void)
{
    ESP_RETURN_ON_ERROR(tas2780_set_mute(true), TAG, "tas mute failed");
    ESP_RETURN_ON_ERROR(tas2780_write_page(0x00), TAG, "tas page failed");
    return i2c_write_reg(TATER_SAT1_TAS2780_I2C_ADDR, 0x02, 0x82);
}

static esp_err_t sat1_xmos_reset_boot(void)
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
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TATER_XMOS_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    return ESP_OK;
}

static void xmos_status_set_version(bool valid, uint8_t major, uint8_t minor, uint8_t patch)
{
    portENTER_CRITICAL(&s_xmos_status_lock);
    s_xmos_status.version_valid = valid;
    s_xmos_status.major = major;
    s_xmos_status.minor = minor;
    s_xmos_status.patch = patch;
    s_xmos_status.progress_percent = valid ? 100 : 0;
    s_xmos_status.update_state = valid ? TATER_XMOS_UPDATE_SKIPPED : TATER_XMOS_UPDATE_ERROR;
    s_xmos_status.update_attempted = false;
    s_xmos_status.update_required = valid
        && !(major == SAT1_XMOS_TARGET_MAJOR && minor == SAT1_XMOS_TARGET_MINOR && patch == SAT1_XMOS_TARGET_PATCH);
    portEXIT_CRITICAL(&s_xmos_status_lock);
}

static esp_err_t spi_init(void)
{
    if (s_spi) {
        return ESP_OK;
    }
    if (!s_spi_mutex) {
        s_spi_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_spi_mutex, ESP_ERR_NO_MEM, TAG, "spi mutex alloc failed");
    }
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = TATER_SAT1_SPI_MOSI,
        .miso_io_num = TATER_SAT1_SPI_MISO,
        .sclk_io_num = TATER_SAT1_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 260,
    };
    esp_err_t err = spi_bus_initialize(SAT1_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 8000000,
        .mode = 3,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << TATER_SAT1_SPI_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cs_cfg), TAG, "spi cs gpio failed");
    gpio_set_level(TATER_SAT1_SPI_CS, 1);
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SAT1_SPI_HOST, &dev_cfg, &s_spi), TAG, "spi add device failed");

    uint8_t warmup = 0;
    spi_transaction_t warmup_tx = {
        .length = 8,
        .tx_buffer = &warmup,
        .rx_buffer = &warmup,
    };
    gpio_set_level(TATER_SAT1_SPI_CS, 0);
    esp_rom_delay_us(1);
    ESP_ERROR_CHECK_WITHOUT_ABORT(spi_device_polling_transmit(s_spi, &warmup_tx));
    esp_rom_delay_us(1);
    gpio_set_level(TATER_SAT1_SPI_CS, 1);

    ESP_LOGI(TAG, "sat1 spi ready clk=%d mosi=%d miso=%d cs=%d mode=3 speed=8MHz", TATER_SAT1_SPI_CLK, TATER_SAT1_SPI_MOSI, TATER_SAT1_SPI_MISO, TATER_SAT1_SPI_CS);
    return ESP_OK;
}

static esp_err_t sat1_spi_transfer(uint8_t *buf, size_t len)
{
    if (!s_spi || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_transaction_t transaction = {
        .length = len * 8,
        .tx_buffer = buf,
        .rx_buffer = buf,
    };
    if (s_spi_mutex) {
        xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
    }
    gpio_set_level(TATER_SAT1_SPI_CS, 0);
    esp_rom_delay_us(1);
    esp_err_t err = spi_device_polling_transmit(s_spi, &transaction);
    esp_rom_delay_us(1);
    gpio_set_level(TATER_SAT1_SPI_CS, 1);
    if (s_spi_mutex) {
        xSemaphoreGive(s_spi_mutex);
    }
    return err;
}

static bool sat1_control_transfer(uint8_t resource_id, uint8_t command, uint8_t *payload, uint8_t payload_len)
{
    if (!s_spi) {
        return false;
    }

    uint8_t send_recv_buf[256 + 3] = {0};
    int status_report_dummies = SAT1_STATUS_REGISTER_LEN - payload_len - 1;
    if (status_report_dummies < 0) {
        status_report_dummies = 0;
    }

    int attempts = 3;
    do {
        memset(send_recv_buf, 0, sizeof(send_recv_buf));
        send_recv_buf[0] = resource_id;
        send_recv_buf[1] = command;
        send_recv_buf[2] = payload_len + ((command & SAT1_CONTROL_CMD_READ_BIT) ? 1 : 0);
        if (payload && payload_len > 0) {
            memcpy(&send_recv_buf[3], payload, payload_len);
        }
        if (sat1_spi_transfer(send_recv_buf, payload_len + 3 + (size_t)status_report_dummies) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (send_recv_buf[0] == SAT1_CONTROL_COMMAND_IGNORED && attempts-- > 0);

    if (send_recv_buf[0] == SAT1_CONTROL_COMMAND_IGNORED) {
        return false;
    }
    if ((send_recv_buf[0] + send_recv_buf[1] + send_recv_buf[2]) == 0) {
        return false;
    }

    if (command & SAT1_CONTROL_CMD_READ_BIT) {
        attempts = 3;
        do {
            memset(send_recv_buf, 0, payload_len + 3);
            if (sat1_spi_transfer(send_recv_buf, payload_len + 3) != ESP_OK) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        } while (send_recv_buf[0] == SAT1_CONTROL_COMMAND_IGNORED && attempts-- > 0);

        if (send_recv_buf[0] == SAT1_CONTROL_COMMAND_IGNORED) {
            return false;
        }
        if (payload && payload_len > 0) {
            memcpy(payload, &send_recv_buf[1], payload_len);
        }
    }

    return true;
}

static esp_err_t sat1_xmos_read_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
    uint8_t version[5] = {0};
    if (!sat1_control_transfer(SAT1_DFU_CONTROLLER_RESID, SAT1_DFU_GET_VERSION, version, sizeof(version))) {
        xmos_status_set_version(false, 0, 0, 0);
        return ESP_FAIL;
    }
    bool nonzero = false;
    for (size_t i = 0; i < sizeof(version); i++) {
        nonzero = nonzero || version[i] != 0;
    }
    if (!nonzero) {
        xmos_status_set_version(false, 0, 0, 0);
        return ESP_FAIL;
    }
    if (major) {
        *major = version[0];
    }
    if (minor) {
        *minor = version[1];
    }
    if (patch) {
        *patch = version[2];
    }
    xmos_status_set_version(true, version[0], version[1], version[2]);
    ESP_LOGI(TAG, "sat1 xmos firmware v%u.%u.%u prerelease=%u.%u", version[0], version[1], version[2], version[3], version[4]);
    return ESP_OK;
}

static void sat1_xmos_read_version_with_retry(void)
{
    for (int attempt = 1; attempt <= 8; attempt++) {
        if (sat1_xmos_read_version(NULL, NULL, NULL) == ESP_OK) {
            return;
        }
        ESP_LOGW(TAG, "sat1 xmos version read failed attempt=%d", attempt);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static esp_err_t sat1_read_doa(tater_audio_doa_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t payload[SAT1_DOA_STATE_PAYLOAD_LEN] = {0};
    if (!sat1_control_transfer(SAT1_DOA_RESOURCE_ID, SAT1_DOA_READ_STATE, payload, sizeof(payload))) {
        return ESP_FAIL;
    }
    memset(out, 0, sizeof(*out));
    out->sample_delay = read_i16_le(&payload[0]);
    out->confidence = payload[2];
    out->valid = (payload[3] & SAT1_DOA_FLAG_VALID) != 0;
    out->four_mic = (payload[3] & SAT1_DOA_FLAG_FOUR_MIC) != 0;
    out->energy = read_u32_le(&payload[4]);
    out->frame_counter = read_u32_le(&payload[8]);
    out->vertical_delay = read_i16_le(&payload[12]);
    out->angle_index = payload[14] % TATER_LED_COUNT;
    out->mic_energy[0] = read_u32_le(&payload[16]);
    out->mic_energy[1] = read_u32_le(&payload[20]);
    out->mic_energy[2] = read_u32_le(&payload[24]);
    out->mic_energy[3] = read_u32_le(&payload[28]);
    return ESP_OK;
}

static void store_doa_snapshot(const tater_audio_doa_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    portENTER_CRITICAL(&s_doa_lock);
    s_doa = *snapshot;
    s_doa_update_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_doa_lock);
}

static void doa_task(void *arg)
{
    (void)arg;
    while (true) {
        tater_audio_doa_t snapshot = {0};
        esp_err_t err = sat1_read_doa(&snapshot);
        if (err == ESP_OK) {
            int64_t now_us = esp_timer_get_time();
            if (!s_doa_have_frame_counter || snapshot.frame_counter != s_doa_last_frame_counter) {
                s_doa_have_frame_counter = true;
                s_doa_last_frame_counter = snapshot.frame_counter;
                s_doa_last_frame_change_us = now_us;
            } else if (snapshot.valid && s_doa_last_frame_change_us > 0 && now_us - s_doa_last_frame_change_us > 500000) {
                snapshot.valid = false;
                snapshot.confidence = 0;
            }
            if (s_doa_failed_reads > 0) {
                ESP_LOGI(TAG, "sat1 doa recovered");
            }
            s_doa_failed_reads = 0;
            store_doa_snapshot(&snapshot);
            if (tater_protocol_voice_active() && now_us - s_doa_last_log_us >= 1000000) {
                s_doa_last_log_us = now_us;
                ESP_LOGI(
                    TAG,
                    "sat1 doa valid=%u four_mic=%u angle=%u x=%d y=%d conf=%u energy=%" PRIu32
                    " mic=%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32,
                    snapshot.valid,
                    snapshot.four_mic,
                    snapshot.angle_index,
                    snapshot.sample_delay,
                    snapshot.vertical_delay,
                    snapshot.confidence,
                    snapshot.energy,
                    snapshot.mic_energy[0],
                    snapshot.mic_energy[1],
                    snapshot.mic_energy[2],
                    snapshot.mic_energy[3]
                );
            }
        } else {
            s_doa_failed_reads++;
            if (s_doa_failed_reads == 1 || (s_doa_failed_reads % 50) == 0) {
                ESP_LOGW(TAG, "sat1 doa read failed count=%lu err=%s", (unsigned long)s_doa_failed_reads, esp_err_to_name(err));
            }
        }
        vTaskDelay(tater_protocol_voice_active() ? pdMS_TO_TICKS(50) : pdMS_TO_TICKS(175));
    }
}

static esp_err_t i2s_init_duplex(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num = SAT1_SPK_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = SAT1_SPK_DMA_FRAME_NUM;
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
            .mclk = TATER_MIC_I2S_MCLK,
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
        "sat1 i2s duplex ready bclk=%d ws=%d mclk=%d din=%d dout=%d",
        TATER_MIC_I2S_BCLK,
        TATER_MIC_I2S_WS,
        TATER_MIC_I2S_MCLK,
        TATER_MIC_I2S_DIN,
        TATER_SPK_I2S_DOUT
    );
    return ESP_OK;
}

esp_err_t tater_audio_i2s_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c init failed");
    ESP_ERROR_CHECK_WITHOUT_ABORT(pcm5122_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(tas2780_init(0));
    ESP_RETURN_ON_ERROR(spi_init(), TAG, "spi init failed");
    ESP_ERROR_CHECK_WITHOUT_ABORT(sat1_xmos_reset_boot());
    sat1_xmos_read_version_with_retry();
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
    v = (v * SAT1_MIC_GAIN_Q8) / 256;
    return clamp_s16(v);
}

static void mic_level_stats(const int16_t *samples, size_t count, uint32_t *peak_out, uint32_t *mean_out)
{
    if (!samples || count == 0) {
        if (peak_out) {
            *peak_out = 0;
        }
        if (mean_out) {
            *mean_out = 0;
        }
        return;
    }
    uint32_t peak = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t sample = samples[i];
        uint32_t magnitude = sample < 0 ? (uint32_t)(-sample) : (uint32_t)sample;
        if (magnitude > peak) {
            peak = magnitude;
        }
        sum += magnitude;
    }
    if (peak_out) {
        *peak_out = peak;
    }
    if (mean_out) {
        *mean_out = (uint32_t)(sum / count);
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
    int16_t mono[TATER_MIC_CHUNK_FRAMES];
    uint32_t active_read_errors = 0;
    uint32_t active_chunks = 0;
    bool last_active = false;

    while (true) {
        bool active = tater_protocol_voice_active() && tater_protocol_is_connected();
        if (active && !last_active) {
            active_read_errors = 0;
            active_chunks = 0;
            ESP_LOGI(TAG, "sat1 mic stream active; waiting for i2s frames");
        }
        last_active = active;

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, rx, sizeof(rx), &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) {
            if (active && (++active_read_errors % 10) == 0) {
                ESP_LOGW(TAG, "sat1 mic i2s read waiting err=%s bytes=%u", esp_err_to_name(err), (unsigned)bytes_read);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t source_frames = bytes_read / (sizeof(int32_t) * TATER_MIC_SOURCE_CHANNELS);
        if (source_frames > TATER_MIC_SOURCE_CHUNK_FRAMES) {
            source_frames = TATER_MIC_SOURCE_CHUNK_FRAMES;
        }
        size_t raw_samples = source_frames * TATER_MIC_SOURCE_CHANNELS;
        size_t out_frames = raw_samples > 3 ? ((raw_samples - 4) / 6) + 1 : 0;
        if (out_frames > TATER_MIC_CHUNK_FRAMES) {
            out_frames = TATER_MIC_CHUNK_FRAMES;
        }

        for (size_t i = 0; i < out_frames; i++) {
            mono[i] = pcm32_to_pcm16_gain(rx[(i * 6) + SAT1_MIC_SAMPLE_PHASE]);
        }

        tater_audio_aec_process_mic(mono, out_frames);
        tater_wake_engine_note_audio(mono, out_frames);
        if (!active) {
            tater_wake_engine_process(mono, out_frames);
        }

        if (active) {
            if (active_chunks < 3) {
                uint32_t peak = 0;
                uint32_t mean = 0;
                mic_level_stats(mono, out_frames, &peak, &mean);
                ESP_LOGI(
                    TAG,
                    "sat1 mic chunk %u frames=%u source_frames=%u bytes=%u peak=%u mean=%u gain_q8=%u phase=%u",
                    (unsigned)(active_chunks + 1),
                    (unsigned)out_frames,
                    (unsigned)source_frames,
                    (unsigned)bytes_read,
                    (unsigned)peak,
                    (unsigned)mean,
                    (unsigned)SAT1_MIC_GAIN_Q8,
                    (unsigned)SAT1_MIC_SAMPLE_PHASE
                );
            }
            active_chunks++;
            tater_protocol_send_audio(mono, out_frames);
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
    if (!s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    reset_speaker_audio_level();
    ESP_ERROR_CHECK_WITHOUT_ABORT(pcm5122_set_mute(true));
    ESP_RETURN_ON_ERROR(tas2780_activate(0), TAG, "tas activate failed");
    if (s_speaker_enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_chan), TAG, "speaker i2s disable failed");
        s_speaker_enabled = false;
    }
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "speaker i2s enable failed");
    s_speaker_enabled = true;
    s_speaker_primed = false;
    return ESP_OK;
}

static void speaker_prime_silence(void)
{
    if (!s_tx_chan || s_speaker_primed) {
        return;
    }

    int32_t zeros[SAT1_SPK_WRITE_FRAMES * TATER_SPK_CHANNELS] = {0};
    size_t byte_count = sizeof(zeros);
    for (uint8_t i = 0; i < SAT1_SPK_DMA_DESC_NUM; i++) {
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, zeros, byte_count, &bytes_written, pdMS_TO_TICKS(2));
        if (err != ESP_OK || bytes_written != byte_count) {
            ESP_LOGD(
                TAG,
                "speaker prime short err=%s bytes=%u/%u",
                esp_err_to_name(err),
                (unsigned)bytes_written,
                (unsigned)byte_count
            );
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
        if (frames > SAT1_SPK_WRITE_FRAMES) {
            frames = SAT1_SPK_WRITE_FRAMES;
        }
        int32_t tx[SAT1_SPK_WRITE_FRAMES * TATER_SPK_CHANNELS];
        for (size_t i = 0; i < frames * TATER_SPK_CHANNELS; i++) {
            tx[i] = ((int32_t)stereo_frames[(offset * TATER_SPK_CHANNELS) + i]) << 16;
        }

        const uint8_t *data = (const uint8_t *)tx;
        size_t byte_count = frames * TATER_SPK_CHANNELS * sizeof(int32_t);
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, data, byte_count, &bytes_written, pdMS_TO_TICKS(25));
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(tas2780_deactivate());
    s_speaker_enabled = false;
    s_speaker_primed = false;
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

#endif
