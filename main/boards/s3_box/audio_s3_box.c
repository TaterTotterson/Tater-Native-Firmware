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
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tater_protocol.h"
#include "wake_engine.h"

#if TATER_BOARD_S3_BOX

static const char *TAG = "tater_audio_s3_box";

#define TATER_I2C_PORT I2C_NUM_0
#define ES7210_I2C_ADDR 0x40
#define ES8311_I2C_ADDR 0x18
#define S3_BOX_WAKE_CHANNEL 0
#define S3_BOX_STREAM_CHANNEL 0
#define S3_BOX_MIC_GAIN_Q8 1024
#define S3_BOX_SPK_DMA_DESC_NUM 4
#define S3_BOX_SPK_DMA_FRAME_NUM 240
#define S3_BOX_SPK_WRITE_FRAMES S3_BOX_SPK_DMA_FRAME_NUM
#define S3_BOX_WAKE_DIAG_PEAK_THRESHOLD 2500U
#define S3_BOX_WAKE_DIAG_INTERVAL_US 2000000
#define S3_BOX_WAKE_DIAG_IDLE_INTERVAL_US 15000000

#define ES7210_RESET_REG00 0x00
#define ES7210_CLOCK_OFF_REG01 0x01
#define ES7210_MAINCLK_REG02 0x02
#define ES7210_LRCK_DIVH_REG04 0x04
#define ES7210_LRCK_DIVL_REG05 0x05
#define ES7210_POWER_DOWN_REG06 0x06
#define ES7210_OSR_REG07 0x07
#define ES7210_MODE_CONFIG_REG08 0x08
#define ES7210_TIME_CONTROL0_REG09 0x09
#define ES7210_TIME_CONTROL1_REG0A 0x0A
#define ES7210_SDP_INTERFACE1_REG11 0x11
#define ES7210_SDP_INTERFACE2_REG12 0x12
#define ES7210_ADC34_HPF2_REG20 0x20
#define ES7210_ADC34_HPF1_REG21 0x21
#define ES7210_ADC12_HPF1_REG22 0x22
#define ES7210_ADC12_HPF2_REG23 0x23
#define ES7210_ANALOG_REG40 0x40
#define ES7210_MIC12_BIAS_REG41 0x41
#define ES7210_MIC34_BIAS_REG42 0x42
#define ES7210_MIC1_GAIN_REG43 0x43
#define ES7210_MIC2_GAIN_REG44 0x44
#define ES7210_MIC3_GAIN_REG45 0x45
#define ES7210_MIC4_GAIN_REG46 0x46
#define ES7210_MIC1_POWER_REG47 0x47
#define ES7210_MIC2_POWER_REG48 0x48
#define ES7210_MIC3_POWER_REG49 0x49
#define ES7210_MIC4_POWER_REG4A 0x4A
#define ES7210_MIC12_POWER_REG4B 0x4B
#define ES7210_MIC34_POWER_REG4C 0x4C

#define ES8311_REG00_RESET 0x00
#define ES8311_REG01_CLK_MANAGER 0x01
#define ES8311_REG02_CLK_MANAGER 0x02
#define ES8311_REG03_CLK_MANAGER 0x03
#define ES8311_REG04_CLK_MANAGER 0x04
#define ES8311_REG05_CLK_MANAGER 0x05
#define ES8311_REG06_CLK_MANAGER 0x06
#define ES8311_REG07_CLK_MANAGER 0x07
#define ES8311_REG08_CLK_MANAGER 0x08
#define ES8311_REG09_SDPIN 0x09
#define ES8311_REG0A_SDPOUT 0x0A
#define ES8311_REG0D_SYSTEM 0x0D
#define ES8311_REG0E_SYSTEM 0x0E
#define ES8311_REG12_SYSTEM 0x12
#define ES8311_REG13_SYSTEM 0x13
#define ES8311_REG14_SYSTEM 0x14
#define ES8311_REG16_ADC 0x16
#define ES8311_REG17_ADC 0x17
#define ES8311_REG1C_ADC 0x1C
#define ES8311_REG31_DAC 0x31
#define ES8311_REG32_DAC 0x32
#define ES8311_REG37_DAC 0x37

static i2s_chan_handle_t s_rx_chan;
static i2s_chan_handle_t s_tx_chan;
static SemaphoreHandle_t s_i2c_mutex;
static SemaphoreHandle_t s_speaker_mutex;
static bool s_speaker_ready;
static bool s_speaker_enabled;
static bool s_speaker_primed;
static bool s_speaker_session_active;
static portMUX_TYPE s_speaker_level_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_speaker_audio_level;
static int64_t s_speaker_level_update_us;

static void speaker_prime_silence(void);
static void speaker_keepalive_silence(void);

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
            .clk_speed = 100000,
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

static esp_err_t i2c_update_reg_bits(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    ESP_RETURN_ON_ERROR(i2c_read_reg(addr, reg, &current), TAG, "i2c read reg 0x%02x failed", reg);
    current = (uint8_t)((current & (uint8_t)~mask) | (value & mask));
    return i2c_write_reg(addr, reg, current);
}

static uint8_t es7210_gain_reg(float mic_gain)
{
    mic_gain += 0.5f;
    if (mic_gain <= 33.0f) {
        return (uint8_t)(mic_gain / 3.0f);
    }
    if (mic_gain < 36.0f) {
        return 12;
    }
    if (mic_gain < 37.0f) {
        return 13;
    }
    return 14;
}

static esp_err_t es7210_configure_mic_gain(float mic_gain)
{
    const uint8_t gain = es7210_gain_reg(mic_gain);
    for (uint8_t reg = ES7210_MIC1_GAIN_REG43; reg <= ES7210_MIC4_GAIN_REG46; reg++) {
        ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, reg, 0x10, 0x00), TAG, "es7210 gain power bit failed");
    }
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC12_POWER_REG4B, 0xff), TAG, "es7210 mic12 power off failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC34_POWER_REG4C, 0xff), TAG, "es7210 mic34 power off failed");

    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_CLOCK_OFF_REG01, 0x0b, 0x00), TAG, "es7210 clock enable failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC12_POWER_REG4B, 0x00), TAG, "es7210 mic12 power failed");
    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_MIC1_GAIN_REG43, 0x10, 0x10), TAG, "es7210 mic1 enable failed");
    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_MIC1_GAIN_REG43, 0x0f, gain), TAG, "es7210 mic1 gain failed");

    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_CLOCK_OFF_REG01, 0x0b, 0x00), TAG, "es7210 clock enable failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC12_POWER_REG4B, 0x00), TAG, "es7210 mic12 power failed");
    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_MIC2_GAIN_REG44, 0x10, 0x10), TAG, "es7210 mic2 enable failed");
    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_MIC2_GAIN_REG44, 0x0f, gain), TAG, "es7210 mic2 gain failed");

    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_CLOCK_OFF_REG01, 0x0b, 0x00), TAG, "es7210 clock enable failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC34_POWER_REG4C, 0x00), TAG, "es7210 mic34 power failed");
    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_MIC3_GAIN_REG45, 0x10, 0x10), TAG, "es7210 mic3 enable failed");
    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_MIC3_GAIN_REG45, 0x0f, gain), TAG, "es7210 mic3 gain failed");

    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_CLOCK_OFF_REG01, 0x0b, 0x00), TAG, "es7210 clock enable failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC34_POWER_REG4C, 0x00), TAG, "es7210 mic34 power failed");
    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_MIC4_GAIN_REG46, 0x10, 0x10), TAG, "es7210 mic4 enable failed");
    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_MIC4_GAIN_REG46, 0x0f, gain), TAG, "es7210 mic4 gain failed");
    return ESP_OK;
}

static esp_err_t es7210_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_RESET_REG00, 0xff), TAG, "es7210 reset failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_RESET_REG00, 0x32), TAG, "es7210 reset failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_CLOCK_OFF_REG01, 0x3f), TAG, "es7210 clock off failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_TIME_CONTROL0_REG09, 0x30), TAG, "es7210 time0 failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_TIME_CONTROL1_REG0A, 0x30), TAG, "es7210 time1 failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_ADC12_HPF2_REG23, 0x2a), TAG, "es7210 hpf failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_ADC12_HPF1_REG22, 0x0a), TAG, "es7210 hpf failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_ADC34_HPF2_REG20, 0x0a), TAG, "es7210 hpf failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_ADC34_HPF1_REG21, 0x2a), TAG, "es7210 hpf failed");
    ESP_RETURN_ON_ERROR(i2c_update_reg_bits(ES7210_I2C_ADDR, ES7210_MODE_CONFIG_REG08, 0x01, 0x00), TAG, "es7210 mode failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_ANALOG_REG40, 0xc3), TAG, "es7210 analog failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC12_BIAS_REG41, 0x70), TAG, "es7210 mic12 bias failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC34_BIAS_REG42, 0x70), TAG, "es7210 mic34 bias failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_SDP_INTERFACE1_REG11, 0x60), TAG, "es7210 i2s format failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_SDP_INTERFACE2_REG12, 0x00), TAG, "es7210 i2s pins failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MAINCLK_REG02, 0xc1), TAG, "es7210 mainclk failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_OSR_REG07, 0x20), TAG, "es7210 osr failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_LRCK_DIVH_REG04, 0x01), TAG, "es7210 lrck h failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_LRCK_DIVL_REG05, 0x00), TAG, "es7210 lrck l failed");
    ESP_RETURN_ON_ERROR(es7210_configure_mic_gain(24.0f), TAG, "es7210 mic gain failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC1_POWER_REG47, 0x08), TAG, "es7210 mic1 power failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC2_POWER_REG48, 0x08), TAG, "es7210 mic2 power failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC3_POWER_REG49, 0x08), TAG, "es7210 mic3 power failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC4_POWER_REG4A, 0x08), TAG, "es7210 mic4 power failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_POWER_DOWN_REG06, 0x04), TAG, "es7210 dll power down failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC12_POWER_REG4B, 0x0f), TAG, "es7210 mic12 power on failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_MIC34_POWER_REG4C, 0x0f), TAG, "es7210 mic34 power on failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_RESET_REG00, 0x71), TAG, "es7210 enable failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES7210_I2C_ADDR, ES7210_RESET_REG00, 0x41), TAG, "es7210 enable failed");
    ESP_LOGI(TAG, "es7210 ready addr=0x%02x sample_rate=%u", ES7210_I2C_ADDR, (unsigned)TATER_MIC_SOURCE_SAMPLE_RATE);
    return ESP_OK;
}

static esp_err_t es8311_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG00_RESET, 0x1f), TAG, "es8311 reset failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG00_RESET, 0x00), TAG, "es8311 reset clear failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG01_CLK_MANAGER, 0x3f), TAG, "es8311 clock enable failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG02_CLK_MANAGER, 0x08), TAG, "es8311 clock div failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG03_CLK_MANAGER, 0x10), TAG, "es8311 adc osr failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG04_CLK_MANAGER, 0x10), TAG, "es8311 dac osr failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG05_CLK_MANAGER, 0x00), TAG, "es8311 div failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG06_CLK_MANAGER, 0x03), TAG, "es8311 bclk div failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG07_CLK_MANAGER, 0x00), TAG, "es8311 lrck h failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG08_CLK_MANAGER, 0xff), TAG, "es8311 lrck l failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG09_SDPIN, 0x0c), TAG, "es8311 sdp in failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG0A_SDPOUT, 0x0c), TAG, "es8311 sdp out failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG14_SYSTEM, 0x1a), TAG, "es8311 mic cfg failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG16_ADC, 0x07), TAG, "es8311 adc gain failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG17_ADC, 0xc8), TAG, "es8311 adc vol failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG32_DAC, 0xbf), TAG, "es8311 dac vol failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG31_DAC, 0x00), TAG, "es8311 dac mute off failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG0D_SYSTEM, 0x01), TAG, "es8311 analog power failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG0E_SYSTEM, 0x02), TAG, "es8311 adc power failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG12_SYSTEM, 0x00), TAG, "es8311 dac power failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG13_SYSTEM, 0x10), TAG, "es8311 output failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG1C_ADC, 0x6a), TAG, "es8311 adc eq failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG37_DAC, 0x08), TAG, "es8311 dac eq failed");
    ESP_RETURN_ON_ERROR(i2c_write_reg(ES8311_I2C_ADDR, ES8311_REG00_RESET, 0x80), TAG, "es8311 power on failed");
    ESP_LOGI(TAG, "es8311 ready addr=0x%02x sample_rate=%u", ES8311_I2C_ADDR, (unsigned)TATER_SPK_SAMPLE_RATE);
    return ESP_OK;
}

static esp_err_t speaker_amp_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TATER_SPK_AMP_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "speaker amp gpio failed");
    gpio_set_level(TATER_SPK_AMP_EN, 0);
    return ESP_OK;
}

static esp_err_t i2s_init_duplex(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = S3_BOX_SPK_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = S3_BOX_SPK_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;
    chan_cfg.intr_priority = 3;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan), TAG, "i2s_new_channel duplex failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = TATER_SPK_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
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
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg), TAG, "i2s tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "i2s rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "i2s tx enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "i2s rx enable failed");
    s_speaker_enabled = true;
    speaker_prime_silence();
    s_speaker_ready = true;
    ESP_LOGI(
        TAG,
        "s3 box i2s duplex ready rate=%u bclk=%d ws=%d mclk=%d din=%d dout=%d",
        (unsigned)TATER_SPK_SAMPLE_RATE,
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
    ESP_RETURN_ON_ERROR(speaker_amp_init(), TAG, "speaker amp init failed");
    ESP_RETURN_ON_ERROR(i2s_init_duplex(), TAG, "i2s init failed");
    ESP_RETURN_ON_ERROR(es7210_init(), TAG, "es7210 init failed");
    ESP_RETURN_ON_ERROR(es8311_init(), TAG, "es8311 init failed");
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

static int16_t mic_sample_gain(int16_t sample)
{
    int32_t v = ((int32_t)sample * S3_BOX_MIC_GAIN_Q8) / 256;
    return clamp_s16(v);
}

static int16_t channel_48k_to_16k(const int16_t *samples, size_t source_frame, size_t channel)
{
    if (channel >= TATER_MIC_SOURCE_CHANNELS) {
        channel = 0;
    }
    int32_t sum = 0;
    for (size_t frame = 0; frame < 3; frame++) {
        size_t base = (source_frame + frame) * TATER_MIC_SOURCE_CHANNELS;
        sum += mic_sample_gain(samples[base + channel]);
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

static void mic_source_channel_stats(
    const int16_t *samples,
    size_t out_frames,
    size_t channel,
    uint32_t *peak_out,
    uint32_t *mean_out
)
{
    uint32_t peak = 0;
    uint64_t sum = 0;
    if (samples) {
        for (size_t i = 0; i < out_frames; i++) {
            int32_t sample = channel_48k_to_16k(samples, i * 3, channel);
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
        *mean_out = out_frames ? (uint32_t)(sum / out_frames) : 0;
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
    int16_t rx[TATER_MIC_SOURCE_CHUNK_FRAMES * TATER_MIC_SOURCE_CHANNELS];
    int16_t wake_mono[TATER_MIC_CHUNK_FRAMES];
    int16_t stream_mono[TATER_MIC_CHUNK_FRAMES];
    uint32_t active_read_errors = 0;
    uint32_t idle_read_errors = 0;
    uint32_t active_chunks = 0;
    uint32_t read_chunks = 0;
    int64_t last_wake_diag_us = 0;
    int64_t last_idle_read_log_us = 0;
    bool last_active = false;

    ESP_LOGI(
        TAG,
        "s3 box audio task started wake_channel=%u stream_channel=%u gain=4x source_rate=%u output_rate=%u",
        (unsigned)S3_BOX_WAKE_CHANNEL,
        (unsigned)S3_BOX_STREAM_CHANNEL,
        (unsigned)TATER_MIC_SOURCE_SAMPLE_RATE,
        (unsigned)TATER_MIC_SAMPLE_RATE
    );

    while (true) {
        bool active = tater_protocol_voice_active() && tater_protocol_is_connected();
        if (active && !last_active) {
            active_read_errors = 0;
            active_chunks = 0;
            ESP_LOGI(TAG, "s3 box mic stream active; waiting for i2s frames");
        }
        last_active = active;

        speaker_keepalive_silence();

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, rx, sizeof(rx), &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) {
            if (active && (++active_read_errors % 10) == 0) {
                ESP_LOGW(TAG, "s3 box mic i2s read waiting err=%s bytes=%u", esp_err_to_name(err), (unsigned)bytes_read);
            } else if (!active) {
                idle_read_errors++;
                int64_t now_us = esp_timer_get_time();
                if (now_us - last_idle_read_log_us >= S3_BOX_WAKE_DIAG_IDLE_INTERVAL_US) {
                    ESP_LOGW(
                        TAG,
                        "s3 box idle mic i2s read waiting err=%s bytes=%u errors=%u",
                        esp_err_to_name(err),
                        (unsigned)bytes_read,
                        (unsigned)idle_read_errors
                    );
                    last_idle_read_log_us = now_us;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t source_frames = bytes_read / (sizeof(int16_t) * TATER_MIC_SOURCE_CHANNELS);
        if (source_frames > TATER_MIC_SOURCE_CHUNK_FRAMES) {
            source_frames = TATER_MIC_SOURCE_CHUNK_FRAMES;
        }
        size_t out_frames = source_frames / 3;
        if (out_frames > TATER_MIC_CHUNK_FRAMES) {
            out_frames = TATER_MIC_CHUNK_FRAMES;
        }
        if (++read_chunks == 1) {
            ESP_LOGI(
                TAG,
                "s3 box first mic rx bytes=%u source_frames=%u out_frames=%u",
                (unsigned)bytes_read,
                (unsigned)source_frames,
                (unsigned)out_frames
            );
        }

        for (size_t i = 0; i < out_frames; i++) {
            wake_mono[i] = channel_48k_to_16k(rx, i * 3, S3_BOX_WAKE_CHANNEL);
            stream_mono[i] = channel_48k_to_16k(rx, i * 3, S3_BOX_STREAM_CHANNEL);
        }

        uint32_t wake_peak = 0;
        uint32_t wake_mean = 0;
        uint32_t stream_peak = 0;
        uint32_t stream_mean = 0;
        mic_level_stats(wake_mono, out_frames, &wake_peak, &wake_mean);
        mic_level_stats(stream_mono, out_frames, &stream_peak, &stream_mean);

        tater_wake_engine_note_audio(wake_mono, out_frames);
        if (!active) {
            int64_t now_us = esp_timer_get_time();
            bool crossed_peak = wake_peak >= S3_BOX_WAKE_DIAG_PEAK_THRESHOLD &&
                now_us - last_wake_diag_us >= S3_BOX_WAKE_DIAG_INTERVAL_US;
            bool periodic_idle = now_us - last_wake_diag_us >= S3_BOX_WAKE_DIAG_IDLE_INTERVAL_US;
            if (crossed_peak || periodic_idle) {
                uint32_t ch0_peak = 0;
                uint32_t ch0_mean = 0;
                uint32_t ch1_peak = 0;
                uint32_t ch1_mean = 0;
                mic_source_channel_stats(rx, out_frames, 0, &ch0_peak, &ch0_mean);
                mic_source_channel_stats(rx, out_frames, 1, &ch1_peak, &ch1_mean);
                ESP_LOGI(
                    TAG,
                    "s3 box wake mic levels selected=%u gain=4x wake=%u/%u stream=%u/%u ch0=%u/%u ch1=%u/%u",
                    (unsigned)S3_BOX_WAKE_CHANNEL,
                    (unsigned)wake_peak,
                    (unsigned)wake_mean,
                    (unsigned)stream_peak,
                    (unsigned)stream_mean,
                    (unsigned)ch0_peak,
                    (unsigned)ch0_mean,
                    (unsigned)ch1_peak,
                    (unsigned)ch1_mean
                );
                last_wake_diag_us = now_us;
            }
            tater_wake_engine_process(wake_mono, out_frames);
        }

        if (active) {
            tater_audio_aec_process_mic(stream_mono, out_frames);
            if (active_chunks < 3) {
                ESP_LOGI(
                    TAG,
                    "s3 box mic chunk %u frames=%u source_frames=%u bytes=%u peak=%u mean=%u channel=%u",
                    (unsigned)(active_chunks + 1),
                    (unsigned)out_frames,
                    (unsigned)source_frames,
                    (unsigned)bytes_read,
                    (unsigned)stream_peak,
                    (unsigned)stream_mean,
                    (unsigned)S3_BOX_STREAM_CHANNEL
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
}

esp_err_t tater_audio_speaker_begin(void)
{
    ESP_RETURN_ON_ERROR(speaker_session_take(), TAG, "speaker session lock failed");
    if (!s_tx_chan) {
        xSemaphoreGive(s_speaker_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    reset_speaker_audio_level();
    if (!s_speaker_enabled) {
        esp_err_t err = i2s_channel_enable(s_tx_chan);
        if (err != ESP_OK) {
            xSemaphoreGive(s_speaker_mutex);
            gpio_set_level(TATER_SPK_AMP_EN, 0);
            ESP_LOGE(TAG, "speaker i2s enable failed: %s", esp_err_to_name(err));
            return err;
        }
        s_speaker_enabled = true;
    }
    s_speaker_primed = false;
    speaker_prime_silence();
    gpio_set_level(TATER_SPK_AMP_EN, 1);
    s_speaker_session_active = true;
    return ESP_OK;
}

static void speaker_prime_silence(void)
{
    if (!s_tx_chan || s_speaker_primed) {
        return;
    }
    int16_t zeros[S3_BOX_SPK_WRITE_FRAMES * TATER_SPK_CHANNELS] = {0};
    size_t byte_count = sizeof(zeros);
    for (uint8_t i = 0; i < S3_BOX_SPK_DMA_DESC_NUM; i++) {
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, zeros, byte_count, &bytes_written, pdMS_TO_TICKS(2));
        if (err != ESP_OK || bytes_written != byte_count) {
            ESP_LOGD(TAG, "speaker prime short err=%s bytes=%u/%u", esp_err_to_name(err), (unsigned)bytes_written, (unsigned)byte_count);
            break;
        }
    }
    s_speaker_primed = true;
}

static void speaker_keepalive_silence(void)
{
    if (!s_tx_chan || !s_speaker_enabled || !s_speaker_mutex) {
        return;
    }
    if (xSemaphoreTake(s_speaker_mutex, 0) != pdTRUE) {
        return;
    }
    if (!s_speaker_session_active) {
        int16_t zeros[S3_BOX_SPK_WRITE_FRAMES * TATER_SPK_CHANNELS] = {0};
        size_t bytes_written = 0;
        (void)i2s_channel_write(s_tx_chan, zeros, sizeof(zeros), &bytes_written, 0);
    }
    xSemaphoreGive(s_speaker_mutex);
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
        if (frames > S3_BOX_SPK_WRITE_FRAMES) {
            frames = S3_BOX_SPK_WRITE_FRAMES;
        }
        const int16_t *tx = &stereo_frames[offset * TATER_SPK_CHANNELS];
        size_t byte_count = frames * TATER_SPK_CHANNELS * sizeof(int16_t);
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
    gpio_set_level(TATER_SPK_AMP_EN, 0);
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
    if (out) {
        memset(out, 0, sizeof(*out));
        out->age_ms = UINT32_MAX;
    }
    return false;
}

bool tater_audio_xmos_status_snapshot(tater_audio_xmos_status_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

esp_err_t tater_audio_sat1_read_buttons(uint8_t *buttons)
{
    if (buttons) {
        *buttons = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tater_audio_xvf3800_control_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tater_audio_xvf3800_set_led_ring(const uint8_t *rgb, size_t led_count)
{
    (void)rgb;
    (void)led_count;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tater_audio_xvf3800_set_mute(bool muted)
{
    (void)muted;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tater_audio_xvf3800_read_mute(bool *muted)
{
    if (muted) {
        *muted = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
