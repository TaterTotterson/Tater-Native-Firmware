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

static const char *TAG = "tater_display_s3_box";

#define LCD_CHUNK_ROWS 20
#define LCD_SPI_CLOCK_HZ (40 * 1000 * 1000)
#define DISPLAY_FEED_POLL_MS 60000
#define DISPLAY_FEED_RESPONSE_MAX 4096

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
static uint8_t s_brightness = 64;
static uint32_t s_state_epoch;
static uint32_t s_render_epoch;
static uint32_t s_animation_tick;
static volatile uint8_t s_feedback_mode;
static volatile uint8_t s_feedback_value;
static volatile uint8_t s_feedback_total;
static volatile int64_t s_feedback_until_us;
static SemaphoreHandle_t s_feed_lock;
static display_feed_t s_feed;

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
    display_http_base(base, sizeof(base));
    if (!base[0]) {
        return false;
    }
    int written = snprintf(out, out_len, "%s/tater-ha/v1/display/feed", base);
    if (written < 0 || (size_t)written >= out_len) {
        return false;
    }
    size_t pos = (size_t)written;
    if (target && target[0]) {
        if (!append_char(out, out_len, &pos, '?')) {
            return false;
        }
        written = snprintf(out + pos, out_len - pos, "target=");
        if (written < 0 || (size_t)written >= out_len - pos) {
            return false;
        }
        pos += (size_t)written;
        if (!append_url_encoded(out, out_len, &pos, target)) {
            return false;
        }
        written = snprintf(out + pos, out_len - pos, "&selector=");
        if (written < 0 || (size_t)written >= out_len - pos) {
            return false;
        }
        pos += (size_t)written;
        if (!append_url_encoded(out, out_len, &pos, target)) {
            return false;
        }
        written = snprintf(out + pos, out_len - pos, "&format=compact");
    } else {
        written = snprintf(out + pos, out_len - pos, "?format=compact");
    }
    return written >= 0 && (size_t)written < out_len - pos;
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

static void render_state_screen(void)
{
    const tater_live_settings_t *settings = tater_live_settings_get();
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
    gpio_set_level(TATER_LCD_BACKLIGHT, s_brightness > 0 ? 1 : 0);
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

esp_err_t tater_leds_init(void)
{
    s_feed_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_feed_lock, ESP_ERR_NO_MEM, TAG, "display feed lock alloc failed");
    display_feed_defaults(&s_feed);
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
    s_brightness = brightness;
    gpio_set_level(TATER_LCD_BACKLIGHT, brightness > 0 ? 1 : 0);
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
