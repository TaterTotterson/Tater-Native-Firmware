#include "leds.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio_i2s.h"
#include "board.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "native_settings.h"
#include "ota_update.h"
#include "tater_protocol.h"

#if TATER_BOARD_S3_BOX

#include "esp32s3/rom/tjpgd.h"

static const char *TAG = "tater_display_s3_box";

#define LCD_CHUNK_ROWS 20
#define LCD_SPI_CLOCK_HZ (40 * 1000 * 1000)
#define DISPLAY_FEED_POLL_MS 60000
#define DISPLAY_FEED_RESPONSE_MAX 4096
#define DISPLAY_EVENT_POLL_MS 4000
#define DISPLAY_EVENT_RESPONSE_MAX 16384
#define DISPLAY_EVENT_IMAGE_WIDTH 296
#define DISPLAY_EVENT_IMAGE_HEIGHT 122
#define DISPLAY_EVENT_JPEG_MAX_BYTES (1024 * 1024)
#define DISPLAY_EVENT_JPEG_WORK_BYTES 4096
#define DISPLAY_EVENT_DECODE_MAX_WIDTH 640
#define DISPLAY_EVENT_DECODE_MAX_HEIGHT 480
#define LCD_BACKLIGHT_PWM_FREQUENCY_HZ 5000
#define LCD_BACKLIGHT_PWM_MAX_DUTY ((1U << LEDC_TIMER_10_BIT) - 1U)

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef struct {
    bool has_stats;
    bool has_clock;
    uint32_t count;
    int64_t updated_us;
    char clock_date[24];
    char clock_time[12];
    char clock_ampm[8];
    char assistant_name[24];
    char temp_out[20];
    char temp_in[20];
    char humidity_out[20];
    char humidity_in[20];
    char wind_speed[20];
    char rain_rate[20];
    char lightning_strikes[20];
} display_feed_t;

typedef struct {
    uint32_t seq;
    uint32_t ttl_seconds;
    int64_t expires_us;
    bool has_image;
    char kind[24];
    char priority[16];
    char title[96];
    char message[320];
    char face_id[96];
} display_notification_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
    uint16_t *pixels;
    uint16_t width;
    uint16_t height;
} display_jpeg_session_t;

static const rgb_t TATER_ORANGE = {227, 36, 0};
static const rgb_t TATER_ORANGE_DIM = {58, 12, 0};
static const rgb_t TATER_DISPLAY_ORANGE = {255, 138, 0};
static const rgb_t TATER_HEADER_BG = {5, 5, 5};
static const rgb_t TATER_PANEL_BG = {11, 11, 11};
static const rgb_t TATER_PANEL_BORDER = {48, 48, 48};
static const rgb_t TATER_TEXT_MUTED = {200, 200, 200};
static const rgb_t TATER_RED = {255, 0, 24};
static const rgb_t TATER_GREEN = {57, 212, 160};
static const rgb_t TATER_BLUE = {30, 90, 255};
static const rgb_t TATER_WHITE = {255, 236, 206};
static const rgb_t TATER_GRAY = {88, 82, 76};
static const rgb_t TATER_VOICE_DEFAULT = {255, 90, 31};

static esp_lcd_panel_io_handle_t s_lcd_io;
static uint16_t *s_fb;
static uint16_t *s_dma;
static bool s_display_ready;
static volatile tater_state_t s_state = TATER_STATE_DISCONNECTED;
static uint8_t s_requested_brightness = 80;
static uint8_t s_applied_brightness = UINT8_MAX;
static bool s_brightness_settings_ready;
static bool s_backlight_pwm_ready;
static uint32_t s_state_epoch;
static uint32_t s_render_epoch;
static uint32_t s_animation_tick;
static volatile uint8_t s_feedback_mode;
static volatile uint8_t s_feedback_value;
static volatile uint8_t s_feedback_total;
static volatile int64_t s_feedback_until_us;
static SemaphoreHandle_t s_feed_lock;
static display_feed_t s_feed;
static SemaphoreHandle_t s_notification_lock;
static display_notification_t s_notification;
static uint16_t *s_notification_image;
static uint32_t s_last_display_event_seq;

static bool screen_night_schedule_active(const tater_live_settings_t *settings)
{
    if (!settings || !settings->screen_night_mode_enabled) {
        return false;
    }
    uint32_t local_seconds = 0;
    if (!tater_live_settings_local_seconds(&local_seconds)) {
        return false;
    }
    uint16_t now_minute = (uint16_t)(local_seconds / 60);
    uint16_t start = settings->screen_night_start_minute;
    uint16_t end = settings->screen_night_end_minute;
    if (start == end) {
        return false;
    }
    if (start < end) {
        return now_minute >= start && now_minute < end;
    }
    return now_minute >= start || now_minute < end;
}

static uint8_t requested_backlight_brightness(void)
{
    if (!s_brightness_settings_ready) {
        return s_requested_brightness;
    }
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (screen_night_schedule_active(settings)) {
        return settings->screen_night_brightness;
    }
    return s_requested_brightness;
}

static void apply_backlight_brightness(uint8_t brightness)
{
    uint8_t next = brightness > 100 ? 100 : brightness;
    if (s_applied_brightness == next) {
        return;
    }
    s_applied_brightness = next;
    if (s_backlight_pwm_ready) {
        uint32_t duty = ((uint32_t)next * LCD_BACKLIGHT_PWM_MAX_DUTY + 50U) / 100U;
        ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
        return;
    }
    gpio_set_level(TATER_LCD_BACKLIGHT, next > 0 ? 1 : 0);
}

static void refresh_backlight_brightness(void)
{
    apply_backlight_brightness(requested_backlight_brightness());
}

static esp_err_t backlight_pwm_init(void)
{
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = LCD_BACKLIGHT_PWM_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        return err;
    }
    ledc_channel_config_t channel_config = {
        .gpio_num = TATER_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        gpio_reset_pin(TATER_LCD_BACKLIGHT);
        gpio_set_direction(TATER_LCD_BACKLIGHT, GPIO_MODE_OUTPUT);
        return err;
    }
    s_backlight_pwm_ready = true;
    s_applied_brightness = UINT8_MAX;
    return ESP_OK;
}

static uint16_t lcd_color(rgb_t color)
{
    uint16_t c = (uint16_t)(((uint16_t)(color.r & 0xf8) << 8) | ((uint16_t)(color.g & 0xfc) << 3) | (color.b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

static uint8_t clamp_u8(float value)
{
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 255.0f) {
        return 255;
    }
    return (uint8_t)value;
}

static rgb_t scale_rgb(rgb_t color, float level)
{
    rgb_t out = {
        .r = clamp_u8((float)color.r * level),
        .g = clamp_u8((float)color.g * level),
        .b = clamp_u8((float)color.b * level),
    };
    return out;
}

static rgb_t blend_rgb(rgb_t a, rgb_t b, float t)
{
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    rgb_t out = {
        .r = clamp_u8(((float)a.r * (1.0f - t)) + ((float)b.r * t)),
        .g = clamp_u8(((float)a.g * (1.0f - t)) + ((float)b.g * t)),
        .b = clamp_u8(((float)a.b * (1.0f - t)) + ((float)b.b * t)),
    };
    return out;
}

static float triangle_wave(uint32_t tick, uint32_t period)
{
    if (period < 2) {
        return 1.0f;
    }
    uint32_t step = tick % period;
    uint32_t half = period / 2;
    if (step > half) {
        step = period - step;
    }
    return half ? (float)step / (float)half : 1.0f;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static bool parse_hex_color(const char *value, rgb_t *out)
{
    if (!value || !out) {
        return false;
    }
    const char *text = value[0] == '#' ? value + 1 : value;
    if (strlen(text) != 6) {
        return false;
    }
    int digits[6] = {0};
    for (int i = 0; i < 6; i++) {
        digits[i] = hex_digit(text[i]);
        if (digits[i] < 0) {
            return false;
        }
    }
    out->r = (uint8_t)((digits[0] << 4) | digits[1]);
    out->g = (uint8_t)((digits[2] << 4) | digits[3]);
    out->b = (uint8_t)((digits[4] << 4) | digits[5]);
    return true;
}

static rgb_t configured_voice_color(const tater_live_settings_t *settings)
{
    rgb_t color = TATER_VOICE_DEFAULT;
    if (settings) {
        parse_hex_color(settings->led_color, &color);
    }
    return color;
}

static const uint8_t *glyph_for(char c)
{
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const uint8_t glyphs[][5] = {
        {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46},
        {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x06, 0x49, 0x49, 0x29, 0x1e},
        {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22},
        {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01},
        {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00},
        {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40},
        {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e},
        {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01}, {0x3f, 0x40, 0x40, 0x40, 0x3f},
        {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
    };
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t percent[5] = {0x23, 0x13, 0x08, 0x64, 0x62};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t bar[5] = {0x00, 0x00, 0x7f, 0x00, 0x00};
    static const uint8_t underscore[5] = {0x40, 0x40, 0x40, 0x40, 0x40};

    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c >= '0' && c <= '9') {
        return glyphs[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return glyphs[10 + (c - 'A')];
    }
    switch (c) {
    case '-':
        return dash;
    case '/':
        return slash;
    case ':':
        return colon;
    case '%':
        return percent;
    case '.':
        return dot;
    case '|':
        return bar;
    case '_':
        return underscore;
    default:
        return blank;
    }
}

static void set_pixel(int x, int y, rgb_t color)
{
    if (!s_fb || x < 0 || y < 0 || x >= TATER_LCD_WIDTH || y >= TATER_LCD_HEIGHT) {
        return;
    }
    s_fb[(y * TATER_LCD_WIDTH) + x] = lcd_color(color);
}

static void fill_rect(int x, int y, int w, int h, rgb_t color)
{
    if (!s_fb || w <= 0 || h <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > TATER_LCD_WIDTH) {
        x1 = TATER_LCD_WIDTH;
    }
    if (y1 > TATER_LCD_HEIGHT) {
        y1 = TATER_LCD_HEIGHT;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    uint16_t c = lcd_color(color);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = &s_fb[yy * TATER_LCD_WIDTH];
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = c;
        }
    }
}

static void draw_char(int x, int y, char c, int scale, rgb_t color)
{
    if (scale < 1) {
        scale = 1;
    }
    const uint8_t *glyph = glyph_for(c);
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                fill_rect(x + (col * scale), y + (row * scale), scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, int scale, rgb_t color)
{
    if (!text) {
        return;
    }
    int cursor = x;
    for (const char *p = text; *p; p++) {
        draw_char(cursor, y, *p, scale, color);
        cursor += 6 * scale;
    }
}

static void draw_centered_text(int y, const char *text, int scale, rgb_t color)
{
    size_t len = text ? strlen(text) : 0;
    int width = (int)len * 6 * scale;
    int x = (TATER_LCD_WIDTH - width) / 2;
    if (x < 0) {
        x = 0;
    }
    draw_text(x, y, text, scale, color);
}

static int scale_for_width(const char *text, int preferred_scale, int max_width)
{
    int scale = preferred_scale < 1 ? 1 : preferred_scale;
    size_t len = text ? strlen(text) : 0;
    while (scale > 1 && (int)len * 6 * scale > max_width) {
        scale--;
    }
    return scale;
}

static void fit_text(char *out, size_t out_len, const char *text, int scale, int max_width)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!text) {
        return;
    }
    size_t max_chars = scale > 0 ? (size_t)(max_width / (6 * scale)) : 0;
    if (max_chars == 0) {
        return;
    }
    if (max_chars >= out_len) {
        max_chars = out_len - 1;
    }
    size_t i = 0;
    for (; i < max_chars && text[i]; i++) {
        char c = text[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[i] = c;
    }
    out[i] = '\0';
}

static void draw_fit_text(int x, int y, const char *text, int preferred_scale, int max_width, rgb_t color)
{
    int scale = scale_for_width(text, preferred_scale, max_width);
    char clipped[40] = {0};
    fit_text(clipped, sizeof(clipped), text, scale, max_width);
    draw_text(x, y, clipped, scale, color);
}

static void draw_right_text(int right_x, int y, const char *text, int scale, rgb_t color)
{
    size_t len = text ? strlen(text) : 0;
    int width = (int)len * 6 * scale;
    int x = right_x - width;
    if (x < 0) {
        x = 0;
    }
    draw_text(x, y, text, scale, color);
}

static void draw_right_fit_text(int right_x, int y, const char *text, int preferred_scale, int max_width, rgb_t color)
{
    int scale = scale_for_width(text, preferred_scale, max_width);
    char clipped[40] = {0};
    fit_text(clipped, sizeof(clipped), text, scale, max_width);
    size_t len = strlen(clipped);
    int width = (int)len * 6 * scale;
    int x = right_x - width;
    if (x < 0) {
        x = 0;
    }
    draw_text(x, y, clipped, scale, color);
}

static void compact_sensor_text(char *out, size_t out_len, const char *value)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!value || !value[0]) {
        strlcpy(out, "--", out_len);
        return;
    }
    size_t n = 0;
    bool last_space = false;
    for (const unsigned char *p = (const unsigned char *)value; *p && n + 1 < out_len; p++) {
        unsigned char c = *p;
        if (c < 32 || c >= 127) {
            continue;
        }
        if (c == ' ') {
            if (n == 0 || last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }
        out[n++] = (char)c;
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
    if (n >= 2 && out[n - 2] == ' ' && (out[n - 1] == 'F' || out[n - 1] == 'C' || out[n - 1] == '%')) {
        out[n - 2] = out[n - 1];
        out[n - 1] = '\0';
    }
    if (out[0] == '\0') {
        strlcpy(out, "--", out_len);
    }
}

static void compact_notification_text(char *out, size_t out_len, const char *value)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!value) {
        return;
    }
    size_t pos = 0;
    bool last_space = true;
    for (const unsigned char *p = (const unsigned char *)value; *p && pos + 1 < out_len; p++) {
        unsigned char c = *p;
        if (c < 32 || c >= 127) {
            if (!last_space && pos + 1 < out_len) {
                out[pos++] = ' ';
                last_space = true;
            }
            continue;
        }
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!last_space && pos + 1 < out_len) {
                out[pos++] = ' ';
                last_space = true;
            }
            continue;
        }
        out[pos++] = (char)c;
        last_space = false;
    }
    while (pos > 0 && out[pos - 1] == ' ') {
        pos--;
    }
    out[pos] = '\0';
}

static void notification_extract_face_id(display_notification_t *notification)
{
    if (!notification) {
        return;
    }
    notification->face_id[0] = '\0';
    const char *face = strcasestr(notification->message, "Face ID:");
    if (face) {
        compact_notification_text(notification->face_id, sizeof(notification->face_id), face);
    }
}

static int draw_wrapped_text(int x, int y, const char *text, int scale, int max_width, int max_lines, rgb_t color)
{
    if (!text || !text[0] || scale < 1 || max_width <= 0 || max_lines <= 0) {
        return 0;
    }
    size_t max_chars = (size_t)(max_width / (6 * scale));
    if (max_chars == 0) {
        return 0;
    }
    const char *cursor = text;
    int lines = 0;
    while (*cursor && lines < max_lines) {
        while (*cursor == ' ') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        size_t remaining = strlen(cursor);
        size_t take = remaining < max_chars ? remaining : max_chars;
        if (remaining > max_chars) {
            size_t break_at = take;
            while (break_at > 0 && cursor[break_at] != ' ') {
                break_at--;
            }
            if (break_at > 0) {
                take = break_at;
            }
        }
        char line[64] = {0};
        if (take >= sizeof(line)) {
            take = sizeof(line) - 1;
        }
        memcpy(line, cursor, take);
        while (take > 0 && line[take - 1] == ' ') {
            line[--take] = '\0';
        }
        draw_text(x, y + (lines * 8 * scale), line, scale, color);
        lines++;
        cursor += take;
        while (*cursor == ' ') {
            cursor++;
        }
    }
    return lines;
}

static void draw_hline(int x, int y, int w, rgb_t color)
{
    fill_rect(x, y, w, 1, color);
}

static void draw_vline(int x, int y, int h, rgb_t color)
{
    fill_rect(x, y, 1, h, color);
}

static void draw_border(rgb_t color)
{
    draw_hline(0, 0, TATER_LCD_WIDTH, color);
    draw_hline(0, TATER_LCD_HEIGHT - 1, TATER_LCD_WIDTH, color);
    draw_vline(0, 0, TATER_LCD_HEIGHT, color);
    draw_vline(TATER_LCD_WIDTH - 1, 0, TATER_LCD_HEIGHT, color);
}

static void draw_panel(int x, int y, int w, int h)
{
    fill_rect(x, y, w, h, TATER_PANEL_BG);
    draw_hline(x, y, w, TATER_PANEL_BORDER);
    draw_hline(x, y + h - 1, w, TATER_PANEL_BORDER);
    draw_vline(x, y, h, TATER_PANEL_BORDER);
    draw_vline(x + w - 1, y, h, TATER_PANEL_BORDER);
}

static void draw_disc(int cx, int cy, int radius, rgb_t color)
{
    int r2 = radius * radius;
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            int dx = x - cx;
            int dy = y - cy;
            if ((dx * dx) + (dy * dy) <= r2) {
                set_pixel(x, y, color);
            }
        }
    }
}

static void draw_ring(int cx, int cy, int radius, int thickness, rgb_t color)
{
    int outer = radius * radius;
    int inner_r = radius - thickness;
    if (inner_r < 0) {
        inner_r = 0;
    }
    int inner = inner_r * inner_r;
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            int dx = x - cx;
            int dy = y - cy;
            int d2 = (dx * dx) + (dy * dy);
            if (d2 <= outer && d2 >= inner) {
                set_pixel(x, y, color);
            }
        }
    }
}

static const char *header_status_text(tater_state_t state)
{
    switch (state) {
    case TATER_STATE_PROVISIONING:
        return "SETUP MODE";
    case TATER_STATE_IDLE:
        return "TATER ONLINE";
    case TATER_STATE_LISTENING:
        return "VOICE ACTIVE";
    case TATER_STATE_THINKING:
        return "THINKING";
    case TATER_STATE_SPEAKING:
        return "REPLYING";
    case TATER_STATE_TOOL_CALL:
        return "TOOL RUNNING";
    case TATER_STATE_TIMER:
        return "TIMER";
    case TATER_STATE_OTA:
        return "UPDATING";
    case TATER_STATE_ERROR:
        return "VOICE ERROR";
    case TATER_STATE_DISCONNECTED:
    default:
        return "CONNECTING";
    }
}

static void draw_tater_header(const char *status)
{
    char assistant_name[24] = "TATER";
    if (s_feed_lock && xSemaphoreTake(s_feed_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (s_feed.assistant_name[0]) {
            strlcpy(assistant_name, s_feed.assistant_name, sizeof(assistant_name));
        }
        xSemaphoreGive(s_feed_lock);
    }
    fill_rect(0, 0, TATER_LCD_WIDTH, 34, TATER_HEADER_BG);
    draw_fit_text(10, 8, assistant_name, 3, 90, TATER_DISPLAY_ORANGE);
    draw_right_text(310, 10, status ? status : "STARTING", 2, TATER_TEXT_MUTED);
}

static void draw_status_footer(const tater_live_settings_t *settings)
{
    char line[48] = {0};
    uint8_t volume = settings ? settings->volume_percent : 80;
    snprintf(line, sizeof(line), "VOL %u%%", volume);
    draw_text(18, 214, line, 2, TATER_GRAY);
    if (settings && settings->muted) {
        draw_right_text(302, 214, "MUTED", 2, TATER_DISPLAY_ORANGE);
    } else {
        draw_right_text(302, 214, "VOICE", 2, TATER_GRAY);
    }
}

static void render_activity_bars(int x, int y, int width, int height, rgb_t color, uint32_t tick)
{
    int bars = 10;
    int gap = 5;
    int bar_w = (width - ((bars - 1) * gap)) / bars;
    if (bar_w < 2) {
        bar_w = 2;
    }
    for (int i = 0; i < bars; i++) {
        uint32_t phase = (uint32_t)((i * 3 + tick) % 18);
        if (phase > 9) {
            phase = 18 - phase;
        }
        float level = 0.18f + ((float)phase / 9.0f) * 0.82f;
        int h = (int)((float)height * level);
        int xx = x + (i * (bar_w + gap));
        fill_rect(xx, y + height - h, bar_w, h, blend_rgb(TATER_ORANGE_DIM, color, level));
    }
}

static void server_host_label(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    const char *url = tater_protocol_server_url();
    if (!url || !url[0]) {
        snprintf(out, out_len, "--");
        return;
    }
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    size_t i = 0;
    while (host[i] && host[i] != '/' && host[i] != ':' && i + 1 < out_len) {
        out[i] = host[i];
        i++;
    }
    out[i] = '\0';
    if (out[0] == '\0') {
        snprintf(out, out_len, "--");
    }
}

static void wifi_label(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(out, out_len, "%d", ap.rssi);
    } else {
        snprintf(out, out_len, "--");
    }
}

static void volume_label(const tater_live_settings_t *settings, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    uint8_t volume = settings ? settings->volume_percent : 80;
    if (settings && settings->muted) {
        snprintf(out, out_len, "MUTE");
    } else {
        snprintf(out, out_len, "%u%%", volume);
    }
}

static void display_feed_defaults(display_feed_t *feed)
{
    if (!feed) {
        return;
    }
    memset(feed, 0, sizeof(*feed));
    strlcpy(feed->clock_date, "", sizeof(feed->clock_date));
    strlcpy(feed->clock_time, "--:--", sizeof(feed->clock_time));
    strlcpy(feed->clock_ampm, "", sizeof(feed->clock_ampm));
    strlcpy(feed->assistant_name, "TATER", sizeof(feed->assistant_name));
    strlcpy(feed->temp_out, "--", sizeof(feed->temp_out));
    strlcpy(feed->temp_in, "--", sizeof(feed->temp_in));
    strlcpy(feed->humidity_out, "--", sizeof(feed->humidity_out));
    strlcpy(feed->humidity_in, "--", sizeof(feed->humidity_in));
    strlcpy(feed->wind_speed, "--", sizeof(feed->wind_speed));
    strlcpy(feed->rain_rate, "--", sizeof(feed->rain_rate));
    strlcpy(feed->lightning_strikes, "--", sizeof(feed->lightning_strikes));
}

static void display_feed_snapshot(display_feed_t *out)
{
    if (!out) {
        return;
    }
    display_feed_defaults(out);
    if (s_feed_lock && xSemaphoreTake(s_feed_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        *out = s_feed;
        xSemaphoreGive(s_feed_lock);
    }
}

static void display_feed_publish(const display_feed_t *feed)
{
    if (!feed || !s_feed_lock) {
        return;
    }
    if (xSemaphoreTake(s_feed_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_feed = *feed;
        xSemaphoreGive(s_feed_lock);
    }
}

static bool append_char(char *out, size_t out_len, size_t *pos, char c)
{
    if (!out || !pos || *pos + 1 >= out_len) {
        return false;
    }
    out[(*pos)++] = c;
    out[*pos] = '\0';
    return true;
}

static bool append_url_encoded(char *out, size_t out_len, size_t *pos, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!value) {
        return true;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            if (!append_char(out, out_len, pos, (char)c)) {
                return false;
            }
        } else if (c == ' ') {
            if (!append_char(out, out_len, pos, '+')) {
                return false;
            }
        } else {
            if (*pos + 3 >= out_len) {
                return false;
            }
            out[(*pos)++] = '%';
            out[(*pos)++] = hex[(c >> 4) & 0x0f];
            out[(*pos)++] = hex[c & 0x0f];
            out[*pos] = '\0';
        }
    }
    return true;
}

static void display_http_base(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    const char *url = tater_protocol_server_url();
    if (!url || !url[0]) {
        return;
    }
    if (strncmp(url, "ws://", 5) == 0) {
        snprintf(out, out_len, "http://%s", url + 5);
    } else if (strncmp(url, "wss://", 6) == 0) {
        snprintf(out, out_len, "https://%s", url + 6);
    } else if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        strlcpy(out, url, out_len);
    } else {
        snprintf(out, out_len, "http://%s", url);
    }

    char *ws_path = strstr(out, "/api/tater/satellite/v1/ws");
    if (ws_path) {
        *ws_path = '\0';
    }
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') {
        out[len - 1] = '\0';
        len--;
    }
}

static void display_target_label(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    const char *room = tater_protocol_room();
    if (room && room[0]) {
        strlcpy(out, room, out_len);
        return;
    }
    const char *device = tater_protocol_device_name();
    if (device && device[0]) {
        strlcpy(out, device, out_len);
    }
}

static bool build_display_feed_url(char *out, size_t out_len, const char *target)
{
    char base[180] = {0};
    char selector[80] = {0};
    display_http_base(base, sizeof(base));
    if (!base[0]) {
        return false;
    }
    const char *device_id = tater_protocol_device_id();
    if (device_id && device_id[0]) {
        snprintf(selector, sizeof(selector), "native:%s", device_id);
    }

    int written = snprintf(out, out_len, "%s/tater-ha/v1/display/feed", base);
    if (written < 0 || (size_t)written >= out_len) {
        return false;
    }
    size_t pos = (size_t)written;
    bool has_query = false;
    if (target && target[0]) {
        if (!append_char(out, out_len, &pos, '?')) {
            return false;
        }
        has_query = true;
        written = snprintf(out + pos, out_len - pos, "target=");
        if (written < 0 || (size_t)written >= out_len - pos) {
            return false;
        }
        pos += (size_t)written;
        if (!append_url_encoded(out, out_len, &pos, target)) {
            return false;
        }
    }
    if (selector[0]) {
        if (!append_char(out, out_len, &pos, has_query ? '&' : '?')) {
            return false;
        }
        has_query = true;
        written = snprintf(out + pos, out_len - pos, "selector=");
        if (written < 0 || (size_t)written >= out_len - pos) {
            return false;
        }
        pos += (size_t)written;
        if (!append_url_encoded(out, out_len, &pos, selector)) {
            return false;
        }
    }
    written = snprintf(out + pos, out_len - pos, "%sformat=compact", has_query ? "&" : "?");
    return written >= 0 && (size_t)written < out_len - pos;
}

static void display_event_target_label(char *out, size_t out_len)
{
    char label[96] = {0};
    display_target_label(label, sizeof(label));
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    bool separator = false;
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)label; *p && pos + 1 < out_len; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '.' || c == '@' || c == '-') {
            out[pos++] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            separator = false;
        } else if (pos > 0 && !separator) {
            out[pos++] = '_';
            separator = true;
        }
    }
    while (pos > 0 && out[pos - 1] == '_') {
        pos--;
    }
    out[pos] = '\0';
}

static bool build_display_events_url(char *out, size_t out_len)
{
    char base[180] = {0};
    char target[96] = {0};
    display_http_base(base, sizeof(base));
    display_event_target_label(target, sizeof(target));
    if (!out || out_len == 0 || !base[0]) {
        return false;
    }
    int written = snprintf(out, out_len, "%s/tater-ha/v1/display/events?after_seq=%lu&limit=4", base, (unsigned long)s_last_display_event_seq);
    if (written < 0 || (size_t)written >= out_len) {
        return false;
    }
    size_t pos = (size_t)written;
    if (target[0]) {
        written = snprintf(out + pos, out_len - pos, "&target=");
        if (written < 0 || (size_t)written >= out_len - pos) {
            return false;
        }
        pos += (size_t)written;
        if (!append_url_encoded(out, out_len, &pos, target)) {
            return false;
        }
    }
    return true;
}

static bool build_display_snapshot_url(char *out, size_t out_len, const char *snapshot_id)
{
    char base[180] = {0};
    if (!out || out_len == 0 || !snapshot_id || !snapshot_id[0]) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)snapshot_id; *p; p++) {
        unsigned char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
    display_http_base(base, sizeof(base));
    if (!base[0]) {
        return false;
    }
    int written = snprintf(out, out_len, "%s/tater-ha/v1/display/snapshots/%s", base, snapshot_id);
    return written >= 0 && (size_t)written < out_len;
}

static void display_feed_copy_text(cJSON *text, const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    strlcpy(out, "--", out_len);
    cJSON *item = text ? cJSON_GetObjectItem(text, key) : NULL;
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        compact_sensor_text(out, out_len, item->valuestring);
    }
}

static esp_err_t parse_display_feed(const char *json, size_t json_len, display_feed_t *feed)
{
    if (!json || !feed) {
        return ESP_ERR_INVALID_ARG;
    }
    display_feed_defaults(feed);
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) {
        return ESP_FAIL;
    }
    cJSON *clock = cJSON_GetObjectItem(root, "clock");
    cJSON *clock_date = clock ? cJSON_GetObjectItem(clock, "date") : NULL;
    cJSON *clock_time = clock ? cJSON_GetObjectItem(clock, "time") : NULL;
    cJSON *clock_ampm = clock ? cJSON_GetObjectItem(clock, "ampm") : NULL;
    if (cJSON_IsString(clock_date) && clock_date->valuestring && clock_date->valuestring[0]) {
        compact_sensor_text(feed->clock_date, sizeof(feed->clock_date), clock_date->valuestring);
    }
    if (cJSON_IsString(clock_time) && clock_time->valuestring && clock_time->valuestring[0]) {
        compact_sensor_text(feed->clock_time, sizeof(feed->clock_time), clock_time->valuestring);
        feed->has_clock = true;
    }
    if (cJSON_IsString(clock_ampm) && clock_ampm->valuestring) {
        compact_sensor_text(feed->clock_ampm, sizeof(feed->clock_ampm), clock_ampm->valuestring);
    }
    cJSON *assistant = cJSON_GetObjectItem(root, "assistant");
    cJSON *assistant_first_name = assistant ? cJSON_GetObjectItem(assistant, "first_name") : NULL;
    if (!cJSON_IsString(assistant_first_name)) {
        assistant_first_name = cJSON_GetObjectItem(root, "assistant_name");
    }
    if (cJSON_IsString(assistant_first_name) && assistant_first_name->valuestring && assistant_first_name->valuestring[0]) {
        compact_sensor_text(feed->assistant_name, sizeof(feed->assistant_name), assistant_first_name->valuestring);
    }

    cJSON *count = cJSON_GetObjectItem(root, "count");
    if (cJSON_IsNumber(count) && count->valuedouble > 0) {
        feed->count = (uint32_t)count->valuedouble;
        feed->has_stats = true;
    }
    cJSON *text = cJSON_GetObjectItem(root, "text");
    display_feed_copy_text(text, "temp_out", feed->temp_out, sizeof(feed->temp_out));
    display_feed_copy_text(text, "temp_in", feed->temp_in, sizeof(feed->temp_in));
    display_feed_copy_text(text, "humidity_out", feed->humidity_out, sizeof(feed->humidity_out));
    display_feed_copy_text(text, "humidity_in", feed->humidity_in, sizeof(feed->humidity_in));
    display_feed_copy_text(text, "wind_speed", feed->wind_speed, sizeof(feed->wind_speed));
    display_feed_copy_text(text, "rain_rate", feed->rain_rate, sizeof(feed->rain_rate));
    display_feed_copy_text(text, "lightning_strikes", feed->lightning_strikes, sizeof(feed->lightning_strikes));
    feed->updated_us = esp_timer_get_time();
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_display_feed_url(const char *url, display_feed_t *feed)
{
    if (!url || !url[0] || !feed) {
        return ESP_ERR_INVALID_ARG;
    }
    char *body = heap_caps_malloc(DISPLAY_FEED_RESPONSE_MAX + 1, MALLOC_CAP_8BIT);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 3000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        return ESP_FAIL;
    }
    const char *token = tater_protocol_token();
    if (token && token[0]) {
        esp_http_client_set_header(client, "X-Tater-Token", token);
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        int total = 0;
        while (total < DISPLAY_FEED_RESPONSE_MAX) {
            int got = esp_http_client_read(client, body + total, DISPLAY_FEED_RESPONSE_MAX - total);
            if (got < 0) {
                err = ESP_FAIL;
                break;
            }
            if (got == 0) {
                break;
            }
            total += got;
        }
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            body[total] = '\0';
            if (status == 200 && total > 0) {
                err = parse_display_feed(body, (size_t)total, feed);
            } else {
                ESP_LOGW(TAG, "display feed status=%d bytes=%d", status, total);
                err = ESP_FAIL;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);
    return err;
}

static bool fetch_display_feed(void)
{
    char target[96] = {0};
    char url[300] = {0};
    display_feed_t feed;
    display_target_label(target, sizeof(target));
    if (target[0] && build_display_feed_url(url, sizeof(url), target) && fetch_display_feed_url(url, &feed) == ESP_OK && feed.has_stats) {
        display_feed_publish(&feed);
        return true;
    }

    if (build_display_feed_url(url, sizeof(url), "") && fetch_display_feed_url(url, &feed) == ESP_OK) {
        display_feed_publish(&feed);
        return feed.has_stats;
    }
    return false;
}

static void *display_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static esp_err_t fetch_http_bytes(const char *url, size_t max_bytes, uint8_t **out_data, size_t *out_len)
{
    if (!url || !url[0] || !out_data || !out_len || max_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }
    const char *token = tater_protocol_token();
    if (token && token[0]) {
        esp_http_client_set_header(client, "X-Tater-Token", token);
    }
    esp_err_t err = esp_http_client_open(client, 0);
    uint8_t *data = NULL;
    size_t total = 0;
    size_t capacity = 0;
    int64_t expected_length = -1;
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int64_t content_length = esp_http_client_get_content_length(client);
        if (status != 200 || content_length > (int64_t)max_bytes) {
            ESP_LOGW(TAG, "display image status=%d content_length=%lld", status, (long long)content_length);
            err = ESP_FAIL;
        } else {
            expected_length = content_length;
            capacity = content_length > 0 ? (size_t)content_length : 64 * 1024;
            if (capacity > max_bytes) {
                capacity = max_bytes;
            }
            if (capacity == 0) {
                capacity = 1;
            }
            data = display_alloc(capacity);
            if (!data) {
                err = ESP_ERR_NO_MEM;
            }
        }
    }
    while (err == ESP_OK) {
        if (expected_length >= 0 && total >= (size_t)expected_length) {
            break;
        }
        if (total == capacity) {
            if (capacity >= max_bytes) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            size_t next_capacity = capacity * 2;
            if (next_capacity > max_bytes) {
                next_capacity = max_bytes;
            }
            uint8_t *grown = heap_caps_realloc(data, next_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) {
                grown = heap_caps_realloc(data, next_capacity, MALLOC_CAP_8BIT);
            }
            if (!grown) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            data = grown;
            capacity = next_capacity;
        }
        int got = esp_http_client_read(client, (char *)data + total, capacity - total);
        if (got < 0) {
            err = ESP_FAIL;
            break;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || total == 0) {
        free(data);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    *out_data = data;
    *out_len = total;
    return ESP_OK;
}

static UINT display_jpeg_input(JDEC *decoder, BYTE *buffer, UINT requested)
{
    display_jpeg_session_t *session = decoder ? (display_jpeg_session_t *)decoder->device : NULL;
    if (!session || session->offset >= session->length) {
        return 0;
    }
    size_t available = session->length - session->offset;
    size_t take = requested < available ? requested : available;
    if (buffer && take > 0) {
        memcpy(buffer, session->data + session->offset, take);
    }
    session->offset += take;
    return (UINT)take;
}

static UINT display_jpeg_output(JDEC *decoder, void *bitmap, JRECT *rect)
{
    display_jpeg_session_t *session = decoder ? (display_jpeg_session_t *)decoder->device : NULL;
    const uint8_t *rgb = (const uint8_t *)bitmap;
    if (!session || !session->pixels || !rgb || !rect) {
        return 0;
    }
    uint16_t block_width = (uint16_t)(rect->right - rect->left + 1);
    uint16_t block_height = (uint16_t)(rect->bottom - rect->top + 1);
    for (uint16_t row = 0; row < block_height; row++) {
        uint16_t y = (uint16_t)(rect->top + row);
        if (y >= session->height) {
            rgb += (size_t)block_width * 3;
            continue;
        }
        for (uint16_t col = 0; col < block_width; col++) {
            uint16_t x = (uint16_t)(rect->left + col);
            if (x < session->width) {
                rgb_t color = {rgb[0], rgb[1], rgb[2]};
                session->pixels[(size_t)y * session->width + x] = lcd_color(color);
            }
            rgb += 3;
        }
    }
    return 1;
}

static void resize_event_image_cover(
    const uint16_t *source,
    uint16_t source_width,
    uint16_t source_height,
    uint16_t *destination
)
{
    if (!source || !destination || source_width == 0 || source_height == 0) {
        return;
    }
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_width = source_width;
    uint32_t crop_height = source_height;
    if ((uint32_t)source_width * DISPLAY_EVENT_IMAGE_HEIGHT > (uint32_t)source_height * DISPLAY_EVENT_IMAGE_WIDTH) {
        crop_width = ((uint32_t)source_height * DISPLAY_EVENT_IMAGE_WIDTH) / DISPLAY_EVENT_IMAGE_HEIGHT;
        crop_x = ((uint32_t)source_width - crop_width) / 2;
    } else {
        crop_height = ((uint32_t)source_width * DISPLAY_EVENT_IMAGE_HEIGHT) / DISPLAY_EVENT_IMAGE_WIDTH;
        crop_y = ((uint32_t)source_height - crop_height) / 2;
    }
    for (uint32_t y = 0; y < DISPLAY_EVENT_IMAGE_HEIGHT; y++) {
        uint32_t source_y = crop_y + ((y * crop_height) / DISPLAY_EVENT_IMAGE_HEIGHT);
        for (uint32_t x = 0; x < DISPLAY_EVENT_IMAGE_WIDTH; x++) {
            uint32_t source_x = crop_x + ((x * crop_width) / DISPLAY_EVENT_IMAGE_WIDTH);
            destination[y * DISPLAY_EVENT_IMAGE_WIDTH + x] = source[source_y * source_width + source_x];
        }
    }
}

static esp_err_t decode_event_jpeg(const uint8_t *jpeg, size_t jpeg_len, uint16_t *destination)
{
    if (!jpeg || jpeg_len == 0 || !destination) {
        return ESP_ERR_INVALID_ARG;
    }
    void *work = heap_caps_malloc(DISPLAY_EVENT_JPEG_WORK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work) {
        return ESP_ERR_NO_MEM;
    }
    display_jpeg_session_t session = {
        .data = jpeg,
        .length = jpeg_len,
    };
    JDEC decoder = {0};
    JRESULT prepared = jd_prepare(&decoder, display_jpeg_input, work, DISPLAY_EVENT_JPEG_WORK_BYTES, &session);
    if (prepared != JDR_OK) {
        ESP_LOGW(TAG, "display snapshot JPEG prepare failed: %d", (int)prepared);
        free(work);
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t scale = 0;
    while (scale < 3 && (((decoder.width + ((1U << scale) - 1U)) >> scale) > DISPLAY_EVENT_DECODE_MAX_WIDTH ||
                         ((decoder.height + ((1U << scale) - 1U)) >> scale) > DISPLAY_EVENT_DECODE_MAX_HEIGHT)) {
        scale++;
    }
    uint32_t decoded_width = (decoder.width + ((1U << scale) - 1U)) >> scale;
    uint32_t decoded_height = (decoder.height + ((1U << scale) - 1U)) >> scale;
    if (decoded_width == 0 || decoded_height == 0 || decoded_width > DISPLAY_EVENT_DECODE_MAX_WIDTH || decoded_height > DISPLAY_EVENT_DECODE_MAX_HEIGHT) {
        free(work);
        return ESP_ERR_INVALID_SIZE;
    }
    session.width = (uint16_t)decoded_width;
    session.height = (uint16_t)decoded_height;
    session.pixels = display_alloc((size_t)decoded_width * decoded_height * sizeof(uint16_t));
    if (!session.pixels) {
        free(work);
        return ESP_ERR_NO_MEM;
    }
    memset(session.pixels, 0, (size_t)decoded_width * decoded_height * sizeof(uint16_t));
    JRESULT decoded = jd_decomp(&decoder, display_jpeg_output, scale);
    esp_err_t err = ESP_OK;
    if (decoded != JDR_OK) {
        ESP_LOGW(TAG, "display snapshot JPEG decode failed: %d", (int)decoded);
        err = ESP_ERR_NOT_SUPPORTED;
    } else {
        resize_event_image_cover(session.pixels, session.width, session.height, destination);
    }
    free(session.pixels);
    free(work);
    return err;
}

static const char *display_json_text(cJSON *object, const char *key)
{
    cJSON *item = object ? cJSON_GetObjectItem(object, key) : NULL;
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static void display_event_snapshot_id(cJSON *event, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    const char *snapshot_id = display_json_text(event, "snapshot_id");
    cJSON *meta = event ? cJSON_GetObjectItem(event, "meta") : NULL;
    cJSON *data = event ? cJSON_GetObjectItem(event, "data") : NULL;
    if (!snapshot_id[0] && cJSON_IsObject(meta)) {
        snapshot_id = display_json_text(meta, "snapshot_id");
    }
    if (!snapshot_id[0] && cJSON_IsObject(data)) {
        snapshot_id = display_json_text(data, "snapshot_id");
    }
    strlcpy(out, snapshot_id, out_len);
}

static esp_err_t parse_display_events(
    const char *json,
    size_t json_len,
    display_notification_t *notification,
    char *snapshot_id,
    size_t snapshot_id_len,
    bool *has_event,
    uint32_t *last_seq
)
{
    if (!json || !notification || !snapshot_id || !has_event || !last_seq) {
        return ESP_ERR_INVALID_ARG;
    }
    *has_event = false;
    *last_seq = s_last_display_event_seq;
    snapshot_id[0] = '\0';
    memset(notification, 0, sizeof(*notification));
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) {
        return ESP_FAIL;
    }
    cJSON *response_last_seq = cJSON_GetObjectItem(root, "last_seq");
    if (cJSON_IsNumber(response_last_seq) && response_last_seq->valuedouble > 0) {
        *last_seq = (uint32_t)response_last_seq->valuedouble;
    }
    cJSON *events = cJSON_GetObjectItem(root, "events");
    int count = cJSON_IsArray(events) ? cJSON_GetArraySize(events) : 0;
    if (count <= 0) {
        cJSON_Delete(root);
        return ESP_OK;
    }
    cJSON *event = cJSON_GetArrayItem(events, count - 1);
    if (!cJSON_IsObject(event)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON *seq = cJSON_GetObjectItem(event, "seq");
    if (cJSON_IsNumber(seq) && seq->valuedouble > 0) {
        notification->seq = (uint32_t)seq->valuedouble;
        if (notification->seq > *last_seq) {
            *last_seq = notification->seq;
        }
    }
    cJSON *ttl = cJSON_GetObjectItem(event, "ttl_seconds");
    int ttl_seconds = cJSON_IsNumber(ttl) ? ttl->valueint : 18;
    if (ttl_seconds < 6) {
        ttl_seconds = 6;
    } else if (ttl_seconds > 90) {
        ttl_seconds = 90;
    }
    notification->ttl_seconds = (uint32_t)ttl_seconds;
    compact_notification_text(notification->kind, sizeof(notification->kind), display_json_text(event, "kind"));
    compact_notification_text(notification->priority, sizeof(notification->priority), display_json_text(event, "priority"));
    compact_notification_text(notification->title, sizeof(notification->title), display_json_text(event, "title"));
    const char *message = display_json_text(event, "message");
    if (!message[0]) {
        message = display_json_text(event, "description");
    }
    compact_notification_text(notification->message, sizeof(notification->message), message);
    if (!notification->kind[0]) {
        strlcpy(notification->kind, "notification", sizeof(notification->kind));
    }
    if (!notification->priority[0]) {
        strlcpy(notification->priority, "normal", sizeof(notification->priority));
    }
    if (!notification->title[0]) {
        strlcpy(notification->title, strcasecmp(notification->kind, "camera") == 0 ? "Camera Event" : "Notification", sizeof(notification->title));
    }
    notification_extract_face_id(notification);
    display_event_snapshot_id(event, snapshot_id, snapshot_id_len);
    *has_event = true;
    cJSON_Delete(root);
    return ESP_OK;
}

static void display_notification_publish(const display_notification_t *notification, const uint16_t *image)
{
    if (!notification || !s_notification_lock) {
        return;
    }
    if (xSemaphoreTake(s_notification_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    s_notification = *notification;
    s_notification.has_image = image && s_notification_image;
    s_notification.expires_us = esp_timer_get_time() + ((int64_t)s_notification.ttl_seconds * 1000000LL);
    if (s_notification.has_image) {
        memcpy(s_notification_image, image, DISPLAY_EVENT_IMAGE_WIDTH * DISPLAY_EVENT_IMAGE_HEIGHT * sizeof(uint16_t));
    }
    xSemaphoreGive(s_notification_lock);
}

static bool fetch_display_events(void)
{
    char url[320] = {0};
    if (!build_display_events_url(url, sizeof(url))) {
        return false;
    }
    uint8_t *response = NULL;
    size_t response_len = 0;
    esp_err_t err = fetch_http_bytes(url, DISPLAY_EVENT_RESPONSE_MAX, &response, &response_len);
    if (err != ESP_OK) {
        return false;
    }
    display_notification_t notification;
    char snapshot_id[96] = {0};
    bool has_event = false;
    uint32_t last_seq = s_last_display_event_seq;
    err = parse_display_events((const char *)response, response_len, &notification, snapshot_id, sizeof(snapshot_id), &has_event, &last_seq);
    free(response);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display event response parse failed");
        return false;
    }
    if (last_seq > s_last_display_event_seq) {
        s_last_display_event_seq = last_seq;
    }
    if (!has_event) {
        return true;
    }

    uint16_t *image = NULL;
    if (snapshot_id[0]) {
        char snapshot_url[320] = {0};
        if (build_display_snapshot_url(snapshot_url, sizeof(snapshot_url), snapshot_id)) {
            uint8_t *jpeg = NULL;
            size_t jpeg_len = 0;
            if (fetch_http_bytes(snapshot_url, DISPLAY_EVENT_JPEG_MAX_BYTES, &jpeg, &jpeg_len) == ESP_OK) {
                image = display_alloc(DISPLAY_EVENT_IMAGE_WIDTH * DISPLAY_EVENT_IMAGE_HEIGHT * sizeof(uint16_t));
                if (!image || decode_event_jpeg(jpeg, jpeg_len, image) != ESP_OK) {
                    free(image);
                    image = NULL;
                }
                free(jpeg);
            }
        }
    }
    display_notification_publish(&notification, image);
    bool image_shown = image != NULL;
    free(image);
    ESP_LOGI(TAG, "display event seq=%lu kind=%s snapshot=%s", (unsigned long)notification.seq, notification.kind, image_shown ? "shown" : "none");
    return true;
}

static void render_info_card(int x, int y, const char *title, const char *value)
{
    draw_panel(x, y, 142, 62);
    draw_text(x + 8, y + 8, title, 2, TATER_TEXT_MUTED);
    draw_fit_text(x + 8, y + 32, value && value[0] ? value : "--", 3, 126, TATER_DISPLAY_ORANGE);
}

static void render_sensor_card(int x, int y, const char *title, const char *primary, const char *secondary)
{
    draw_panel(x, y, 142, 62);
    draw_text(x + 8, y + 8, title, 2, TATER_TEXT_MUTED);
    draw_fit_text(x + 8, y + 32, primary && primary[0] ? primary : "--", 3, 90, TATER_DISPLAY_ORANGE);
    draw_right_fit_text(x + 132, y + 38, secondary && secondary[0] ? secondary : "--", 2, 40, TATER_WHITE);
}

static void render_small_card(int x, int y, int width, const char *title, const char *value)
{
    draw_panel(x, y, width, 28);
    draw_fit_text(x + 6, y + 4, title, 1, width - 12, TATER_TEXT_MUTED);
    draw_fit_text(x + 6, y + 16, value && value[0] ? value : "--", 1, width - 12, TATER_WHITE);
}

static void render_small_sensor_card(int x, int y, int width, const char *title, const char *value)
{
    draw_panel(x, y, width, 28);
    draw_fit_text(x + 6, y + 8, title, 1, 34, TATER_TEXT_MUTED);
    draw_right_fit_text(x + width - 8, y + 8, value && value[0] ? value : "--", 1, width - 44, TATER_WHITE);
}

static void render_home_dashboard(const tater_live_settings_t *settings)
{
    fill_rect(0, 0, TATER_LCD_WIDTH, TATER_LCD_HEIGHT, (rgb_t){0, 0, 0});
    draw_tater_header(header_status_text(s_state));

    display_feed_t feed;
    display_feed_snapshot(&feed);
    if (feed.has_stats) {
        if (feed.clock_date[0]) {
            draw_centered_text(42, feed.clock_date, scale_for_width(feed.clock_date, 2, 300), TATER_TEXT_MUTED);
        }
        draw_centered_text(62, feed.has_clock ? feed.clock_time : "--:--", 7, TATER_WHITE);
        if (feed.clock_ampm[0]) {
            draw_right_fit_text(310, 88, feed.clock_ampm, 2, 38, TATER_TEXT_MUTED);
        }
        render_sensor_card(12, 128, "INSIDE", feed.temp_in, feed.humidity_in);
        render_sensor_card(166, 128, "OUTSIDE", feed.temp_out, feed.humidity_out);
        render_small_sensor_card(12, 202, 90, "WIND", feed.wind_speed);
        render_small_sensor_card(114, 202, 90, "RAIN", feed.rain_rate);
        render_small_sensor_card(216, 202, 92, "STORM", feed.lightning_strikes);
        return;
    }

    char server[40] = {0};
    char wifi[20] = {0};
    char volume[12] = {0};
    server_host_label(server, sizeof(server));
    wifi_label(wifi, sizeof(wifi));
    volume_label(settings, volume, sizeof(volume));

    const char *device_name = tater_protocol_device_name();
    const char *room = tater_protocol_room();
    const char *date_text = feed.clock_date[0] ? feed.clock_date : TATER_FIRMWARE_VERSION;
    const char *clock_text = feed.has_clock ? feed.clock_time : (tater_protocol_is_connected() ? "ONLINE" : "WAIT");
    if (s_state == TATER_STATE_PROVISIONING) {
        date_text = "TATER-SETUP";
        clock_text = "SETUP";
    } else if (s_state == TATER_STATE_DISCONNECTED) {
        clock_text = feed.has_clock ? feed.clock_time : "WAIT";
    }

    draw_centered_text(44, date_text, scale_for_width(date_text, 2, 300), TATER_TEXT_MUTED);
    draw_centered_text(62, clock_text, 7, TATER_WHITE);
    if (!feed.has_stats && feed.has_clock && feed.clock_ampm[0]) {
        draw_text(252, 88, feed.clock_ampm, 2, TATER_TEXT_MUTED);
    }
    render_info_card(12, 128, "SERVER", server);
    render_info_card(166, 128, room && room[0] ? "ROOM" : "DEVICE", room && room[0] ? room : device_name);
    render_small_card(12, 202, 90, "WAKE", settings ? settings->wake_word : "HEY");
    render_small_card(114, 202, 90, "VOL", volume);
    render_small_card(216, 202, 92, "WIFI", wifi);
}

static const char *voice_title_text(tater_state_t state)
{
    switch (state) {
    case TATER_STATE_LISTENING:
        return "LISTENING";
    case TATER_STATE_THINKING:
        return "THINKING";
    case TATER_STATE_SPEAKING:
        return "REPLYING";
    case TATER_STATE_TOOL_CALL:
        return "TOOL CALL";
    case TATER_STATE_TIMER:
        return "TIMER";
    case TATER_STATE_OTA:
        return "UPDATING";
    case TATER_STATE_ERROR:
        return "VOICE ERROR";
    case TATER_STATE_PROVISIONING:
        return "SETUP MODE";
    case TATER_STATE_IDLE:
        return "READY";
    case TATER_STATE_DISCONNECTED:
    default:
        return "CONNECTING";
    }
}

static const char *voice_subtitle_text(tater_state_t state)
{
    switch (state) {
    case TATER_STATE_LISTENING:
        return "SAY IT";
    case TATER_STATE_THINKING:
        return "WORKING";
    case TATER_STATE_SPEAKING:
        return "SENDING ANSWER";
    case TATER_STATE_TOOL_CALL:
        return "RUNNING TOOL";
    case TATER_STATE_TIMER:
        return "RINGING";
    case TATER_STATE_OTA:
        return "KEEP POWERED";
    case TATER_STATE_ERROR:
        return "NEEDS ATTENTION";
    case TATER_STATE_DISCONNECTED:
    default:
        return "CONNECTING TO TATER";
    }
}

static const char *voice_motion_text(tater_state_t state)
{
    static const char * const listening[] = {
        "O        .        O",
        "  O      .      O  ",
        "    O    .    O    ",
        "      O  .  O      ",
        "        O.O        ",
        "      O  .  O      ",
    };
    static const char * const thinking[] = {
        "O  --------    ",
        "  O  ------    ",
        "    O  ----    ",
        "      O  --    ",
        "        O      ",
        "      --  O    ",
    };
    static const char * const speaking[] = {
        "--__--__--__--",
        "__--__--__--__",
        "-_--_--_--_--",
        "_--_--_--_--_",
        "--__--__--__--",
        "__--__--__--__",
    };
    static const char * const tool[] = {
        "--  --  --  --",
        "  --  --  --  ",
        "    --  --    ",
        "  --  --  --  ",
        "--  --  --  --",
        "----      ----",
    };
    static const char * const error[] = {
        "X      X      X",
        "  X    X    X  ",
        "    X  X  X    ",
        "      XXX      ",
        "    X  X  X    ",
        "  X    X    X  ",
    };
    const uint32_t frame = (s_animation_tick / 2) % 6;
    switch (state) {
    case TATER_STATE_LISTENING:
        return listening[frame];
    case TATER_STATE_THINKING:
    case TATER_STATE_OTA:
        return thinking[frame];
    case TATER_STATE_SPEAKING:
        return speaking[frame];
    case TATER_STATE_TOOL_CALL:
        return tool[frame];
    case TATER_STATE_ERROR:
        return error[frame];
    case TATER_STATE_TIMER:
        return "O     TIMER     O";
    default:
        return listening[frame];
    }
}

static void render_voice_screen(const tater_live_settings_t *settings)
{
    rgb_t accent = configured_voice_color(settings);
    if (s_state == TATER_STATE_ERROR || s_state == TATER_STATE_TIMER) {
        accent = TATER_RED;
    } else if (s_state == TATER_STATE_OTA || s_state == TATER_STATE_DISCONNECTED) {
        accent = TATER_DISPLAY_ORANGE;
    }

    fill_rect(0, 0, TATER_LCD_WIDTH, TATER_LCD_HEIGHT, (rgb_t){0, 0, 0});
    draw_tater_header(header_status_text(s_state));

    const char *title = voice_title_text(s_state);
    int title_scale = strlen(title) > 9 ? 4 : 5;
    draw_centered_text(54, title, title_scale, accent);

    draw_centered_text(90, voice_motion_text(s_state), 3, accent);
    fill_rect(58, 112, 204, 4, TATER_PANEL_BG);
    fill_rect(82, 126, 156, 2, accent);

    const char *subtitle = voice_subtitle_text(s_state);
    int subtitle_scale = strlen(subtitle) > 14 ? 2 : 3;
    draw_centered_text(150, subtitle, subtitle_scale, TATER_WHITE);

    if (s_state == TATER_STATE_SPEAKING) {
        rgb_t level_color = blend_rgb(accent, TATER_WHITE, tater_audio_speaker_level());
        render_activity_bars(58, 178, 204, 28, level_color, s_animation_tick);
    } else if (s_state == TATER_STATE_LISTENING) {
        render_activity_bars(72, 178, 176, 24, accent, s_animation_tick);
    } else if (s_state == TATER_STATE_OTA) {
        float pulse = 0.30f + (triangle_wave(s_animation_tick, 24) * 0.70f);
        fill_rect(95, 184, 130, 8, TATER_ORANGE_DIM);
        fill_rect(95, 184, (int)(130.0f * pulse), 8, accent);
    }

    draw_status_footer(settings);
}

static bool render_notification_screen(void)
{
    if (!s_notification_lock || xSemaphoreTake(s_notification_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    int64_t now_us = esp_timer_get_time();
    if (s_notification.expires_us <= now_us) {
        memset(&s_notification, 0, sizeof(s_notification));
        xSemaphoreGive(s_notification_lock);
        return false;
    }

    display_notification_t notification = s_notification;
    rgb_t accent = (strcasecmp(notification.priority, "high") == 0 || strcasecmp(notification.priority, "critical") == 0)
        ? TATER_RED
        : TATER_DISPLAY_ORANGE;
    fill_rect(0, 0, TATER_LCD_WIDTH, TATER_LCD_HEIGHT, (rgb_t){0, 0, 0});
    draw_tater_header("AWARENESS");

    if (notification.has_image && s_notification_image) {
        draw_panel(11, 41, DISPLAY_EVENT_IMAGE_WIDTH + 2, DISPLAY_EVENT_IMAGE_HEIGHT + 2);
        for (int y = 0; y < DISPLAY_EVENT_IMAGE_HEIGHT; y++) {
            memcpy(
                &s_fb[(size_t)(42 + y) * TATER_LCD_WIDTH + 12],
                &s_notification_image[(size_t)y * DISPLAY_EVENT_IMAGE_WIDTH],
                DISPLAY_EVENT_IMAGE_WIDTH * sizeof(uint16_t)
            );
        }
        draw_fit_text(12, 170, notification.title, 2, 296, accent);
        draw_wrapped_text(12, 190, notification.message, 2, 296, notification.face_id[0] ? 2 : 3, TATER_WHITE);
        if (notification.face_id[0]) {
            draw_fit_text(12, 226, notification.face_id, 1, 296, accent);
        }
    } else {
        draw_panel(12, 48, 296, 180);
        draw_fit_text(24, 62, notification.title, 3, 272, accent);
        fill_rect(24, 92, 272, 3, TATER_ORANGE_DIM);
        draw_wrapped_text(24, 108, notification.message, 2, 272, notification.face_id[0] ? 6 : 7, TATER_WHITE);
        if (notification.face_id[0]) {
            draw_fit_text(24, 214, notification.face_id, 1, 272, accent);
        }
    }
    xSemaphoreGive(s_notification_lock);
    return true;
}

static void render_state_screen(void)
{
    const tater_live_settings_t *settings = tater_live_settings_get();
    if ((s_state == TATER_STATE_IDLE || s_state == TATER_STATE_DISCONNECTED) && render_notification_screen()) {
        return;
    }
    if (s_state == TATER_STATE_IDLE || s_state == TATER_STATE_DISCONNECTED || s_state == TATER_STATE_PROVISIONING) {
        render_home_dashboard(settings);
    } else {
        render_voice_screen(settings);
    }
}

static void render_feedback_screen(void)
{
    uint8_t mode = s_feedback_mode;
    uint8_t value = s_feedback_value;
    uint8_t total = s_feedback_total ? s_feedback_total : 1;
    if (mode == 0) {
        return;
    }
    if (mode >= 4 && esp_timer_get_time() >= s_feedback_until_us) {
        s_feedback_mode = 0;
        render_state_screen();
        return;
    }

    fill_rect(0, 0, TATER_LCD_WIDTH, TATER_LCD_HEIGHT, (rgb_t){0, 0, 0});
    draw_border(TATER_ORANGE);

    if (mode == 1) {
        draw_centered_text(42, "SETUP RESET", 3, TATER_ORANGE);
        draw_centered_text(82, "CLICK SEQUENCE", 2, TATER_GRAY);
        int lit = (int)(((uint16_t)value * 220U) / total);
        fill_rect(50, 140, 220, 18, TATER_ORANGE_DIM);
        fill_rect(50, 140, lit, 18, TATER_ORANGE);
        char text[24] = {0};
        snprintf(text, sizeof(text), "%u/%u", value, total);
        draw_centered_text(178, text, 3, TATER_WHITE);
    } else if (mode == 2) {
        draw_centered_text(42, "HOLD TO RESET", 3, TATER_ORANGE);
        int lit = (int)(((uint16_t)value * 220U) / total);
        fill_rect(50, 140, 220, 18, TATER_ORANGE_DIM);
        fill_rect(50, 140, lit, 18, value <= 1 ? TATER_RED : TATER_ORANGE);
        char text[24] = {0};
        snprintf(text, sizeof(text), "%u", value);
        draw_centered_text(178, text, 4, value <= 1 ? TATER_RED : TATER_WHITE);
    } else if (mode == 3) {
        draw_centered_text(66, "SETUP MODE", 3, TATER_GREEN);
        draw_centered_text(124, "RESTARTING", 3, TATER_WHITE);
    } else if (mode == 4) {
        draw_centered_text(48, "VOLUME", 3, TATER_ORANGE);
        int lit = (int)(((uint16_t)value * 220U) / 100U);
        fill_rect(50, 132, 220, 24, TATER_ORANGE_DIM);
        fill_rect(50, 132, lit, 24, TATER_ORANGE);
        char text[24] = {0};
        snprintf(text, sizeof(text), "%u%%", value);
        draw_centered_text(178, text, 4, TATER_WHITE);
    } else if (mode == 5) {
        bool muted = value != 0;
        draw_centered_text(66, muted ? "MIC MUTED" : "MIC ACTIVE", 3, muted ? TATER_ORANGE : TATER_GREEN);
        draw_ring(160, 150, 44, 8, muted ? TATER_ORANGE : TATER_GREEN);
    }
}

static esp_err_t lcd_send_cmd(uint8_t cmd)
{
    return esp_lcd_panel_io_tx_param(s_lcd_io, cmd, NULL, 0);
}

static esp_err_t lcd_send_data(const void *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_OK;
    }
    return esp_lcd_panel_io_tx_param(s_lcd_io, -1, data, len);
}

static esp_err_t lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4] = {0};
    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x2a), TAG, "lcd caset failed");
    data[0] = (uint8_t)(x0 >> 8);
    data[1] = (uint8_t)x0;
    data[2] = (uint8_t)(x1 >> 8);
    data[3] = (uint8_t)x1;
    ESP_RETURN_ON_ERROR(lcd_send_data(data, sizeof(data)), TAG, "lcd caset data failed");
    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x2b), TAG, "lcd raset failed");
    data[0] = (uint8_t)(y0 >> 8);
    data[1] = (uint8_t)y0;
    data[2] = (uint8_t)(y1 >> 8);
    data[3] = (uint8_t)y1;
    ESP_RETURN_ON_ERROR(lcd_send_data(data, sizeof(data)), TAG, "lcd raset data failed");
    ESP_RETURN_ON_ERROR(lcd_send_cmd(0x2c), TAG, "lcd ramwr failed");
    return ESP_OK;
}

static esp_err_t lcd_wait_idle(void)
{
    return lcd_send_cmd(0x00);
}

static esp_err_t lcd_flush(void)
{
    if (!s_fb || !s_dma) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int y = 0; y < TATER_LCD_HEIGHT; y += LCD_CHUNK_ROWS) {
        int rows = TATER_LCD_HEIGHT - y;
        if (rows > LCD_CHUNK_ROWS) {
            rows = LCD_CHUNK_ROWS;
        }
        size_t bytes = (size_t)rows * TATER_LCD_WIDTH * sizeof(uint16_t);
        ESP_RETURN_ON_ERROR(lcd_wait_idle(), TAG, "lcd wait before dma reuse failed");
        memcpy(s_dma, &s_fb[y * TATER_LCD_WIDTH], bytes);
        ESP_RETURN_ON_ERROR(lcd_set_window(0, (uint16_t)y, TATER_LCD_WIDTH - 1, (uint16_t)(y + rows - 1)), TAG, "lcd window failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(s_lcd_io, -1, s_dma, bytes), TAG, "lcd pixel data failed");
    }
    ESP_RETURN_ON_ERROR(lcd_wait_idle(), TAG, "lcd final wait failed");
    return ESP_OK;
}

static esp_err_t lcd_init_sequence(void)
{
    typedef struct {
        uint8_t cmd;
        uint8_t data[16];
        uint8_t len;
        uint16_t delay_ms;
    } lcd_init_cmd_t;
    static const lcd_init_cmd_t init[] = {
        {0xEF, {0x03, 0x80, 0x02}, 3, 0},
        {0xCF, {0x00, 0xC1, 0x30}, 3, 0},
        {0xED, {0x64, 0x03, 0x12, 0x81}, 4, 0},
        {0xE8, {0x85, 0x00, 0x78}, 3, 0},
        {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
        {0xF7, {0x20}, 1, 0},
        {0xEA, {0x00, 0x00}, 2, 0},
        {0xC0, {0x23}, 1, 0},
        {0xC1, {0x10}, 1, 0},
        {0xC5, {0x3E, 0x28}, 2, 0},
        {0xC7, {0x86}, 1, 0},
        {0x37, {0x00}, 1, 0},
        {0xB1, {0x00, 0x18}, 2, 0},
        {0xB6, {0x08, 0x82, 0x27}, 3, 0},
        {0xF2, {0x00}, 1, 0},
        {0x26, {0x01}, 1, 0},
        {0xE0, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 15, 0},
        {0xE1, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 15, 0},
        {0x11, {0}, 0, 120},
        {0x20, {0}, 0, 0},
        {0x36, {0xC8}, 1, 0},
        {0x3A, {0x55}, 1, 0},
        {0x29, {0}, 0, 20},
    };
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); i++) {
        ESP_RETURN_ON_ERROR(lcd_send_cmd(init[i].cmd), TAG, "lcd init cmd 0x%02x failed", init[i].cmd);
        if (init[i].len > 0) {
            ESP_RETURN_ON_ERROR(lcd_send_data(init[i].data, init[i].len), TAG, "lcd init data 0x%02x failed", init[i].cmd);
        }
        if (init[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(init[i].delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t lcd_init(void)
{
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << TATER_LCD_RESET) | (1ULL << TATER_LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_cfg), TAG, "lcd gpio config failed");
    gpio_set_level(TATER_LCD_BACKLIGHT, 0);
    gpio_set_level(TATER_LCD_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TATER_LCD_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TATER_LCD_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t buscfg = {
        .mosi_io_num = TATER_LCD_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = TATER_LCD_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TATER_LCD_WIDTH * LCD_CHUNK_ROWS * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(TATER_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "lcd spi bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = TATER_LCD_CS,
        .dc_gpio_num = TATER_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_SPI_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TATER_LCD_SPI_HOST, &io_config, &s_lcd_io), TAG, "lcd io failed");
    ESP_RETURN_ON_ERROR(lcd_init_sequence(), TAG, "lcd init sequence failed");
    esp_err_t backlight_err = backlight_pwm_init();
    if (backlight_err != ESP_OK) {
        ESP_LOGW(TAG, "lcd backlight PWM unavailable; using on/off fallback: %s", esp_err_to_name(backlight_err));
    }
    refresh_backlight_brightness();
    return ESP_OK;
}

static void render(void)
{
    if (s_render_epoch != s_state_epoch) {
        s_render_epoch = s_state_epoch;
        s_animation_tick = 0;
    }
    if (s_feedback_mode) {
        render_feedback_screen();
    } else {
        render_state_screen();
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_flush());
    s_animation_tick++;
}

static void display_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_display_ready) {
            refresh_backlight_brightness();
            render();
        }
        uint32_t delay_ms = 100;
        if (s_state == TATER_STATE_SPEAKING || s_state == TATER_STATE_LISTENING || s_state == TATER_STATE_TOOL_CALL) {
            delay_ms = 60;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void display_feed_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));
    while (true) {
        if (s_display_ready && tater_protocol_is_connected() && !tater_ota_is_running()) {
            bool ok = fetch_display_feed();
            ESP_LOGI(TAG, "display feed refresh %s", ok ? "ok" : "empty");
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_FEED_POLL_MS));
    }
}

static void display_event_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (true) {
        if (s_display_ready && tater_protocol_is_connected() && !tater_ota_is_running()) {
            if (!fetch_display_events()) {
                ESP_LOGD(TAG, "display event refresh failed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_EVENT_POLL_MS));
    }
}

esp_err_t tater_leds_init(void)
{
    s_feed_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_feed_lock, ESP_ERR_NO_MEM, TAG, "display feed lock alloc failed");
    s_notification_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_notification_lock, ESP_ERR_NO_MEM, TAG, "display notification lock alloc failed");
    display_feed_defaults(&s_feed);
    memset(&s_notification, 0, sizeof(s_notification));
    s_notification_image = display_alloc(DISPLAY_EVENT_IMAGE_WIDTH * DISPLAY_EVENT_IMAGE_HEIGHT * sizeof(uint16_t));
    ESP_RETURN_ON_FALSE(s_notification_image, ESP_ERR_NO_MEM, TAG, "display notification image alloc failed");
    s_fb = heap_caps_malloc(TATER_LCD_WIDTH * TATER_LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        s_fb = heap_caps_malloc(TATER_LCD_WIDTH * TATER_LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_NO_MEM, TAG, "lcd framebuffer alloc failed");
    s_dma = heap_caps_malloc(TATER_LCD_WIDTH * LCD_CHUNK_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_dma, ESP_ERR_NO_MEM, TAG, "lcd dma buffer alloc failed");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "lcd init failed");
    s_display_ready = true;
    render_state_screen();
    ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_flush());
    xTaskCreatePinnedToCore(display_task, "tater_display", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(display_feed_task, "tater_display_feed", 6144, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(display_event_task, "tater_display_event", 8192, NULL, 3, NULL, 0);
    return ESP_OK;
}

void tater_leds_set_state(tater_state_t state)
{
    if (s_state != state) {
        s_state = state;
        s_state_epoch++;
    }
}

void tater_leds_set_brightness(uint8_t brightness)
{
    s_requested_brightness = brightness > 100 ? 100 : brightness;
    s_brightness_settings_ready = true;
    refresh_backlight_brightness();
}

void tater_leds_show_setup_reset_clicks(uint8_t clicks, uint8_t required_clicks)
{
    s_feedback_value = clicks;
    s_feedback_total = required_clicks ? required_clicks : 1;
    s_feedback_mode = 1;
}

void tater_leds_show_setup_reset_countdown(uint8_t remaining_steps, uint8_t total_steps)
{
    s_feedback_value = remaining_steps;
    s_feedback_total = total_steps ? total_steps : 1;
    s_feedback_mode = 2;
}

void tater_leds_show_setup_reset_success(void)
{
    s_feedback_value = 0;
    s_feedback_total = 0;
    s_feedback_mode = 3;
}

void tater_leds_clear_setup_reset_feedback(void)
{
    if (s_feedback_mode <= 3) {
        s_feedback_mode = 0;
        s_feedback_value = 0;
        s_feedback_total = 0;
    }
}

void tater_leds_show_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    s_feedback_value = volume_percent;
    s_feedback_total = 100;
    s_feedback_until_us = esp_timer_get_time() + 1200000;
    s_feedback_mode = 4;
}

void tater_leds_show_mute(bool muted)
{
    s_feedback_value = muted ? 1 : 0;
    s_feedback_total = 1;
    s_feedback_until_us = esp_timer_get_time() + 1600000;
    s_feedback_mode = 5;
}

#endif
