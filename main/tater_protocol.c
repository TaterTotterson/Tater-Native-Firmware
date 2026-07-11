#include "tater_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "board.h"
#include "cJSON.h"
#include "esp_core_dump.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "audio_aec.h"
#include "audio_i2s.h"
#include "native_settings.h"
#include "playback.h"
#include "wake_engine.h"

static const char *TAG = "tater_proto";
static const char *NATIVE_WS_PATH = "/api/tater/satellite/v1/ws";

#ifndef TATER_CAP_LINE_OUT
#define TATER_CAP_LINE_OUT false
#endif

#ifndef TATER_CAP_XMOS
#define TATER_CAP_XMOS false
#endif

static esp_websocket_client_handle_t s_client;
static SemaphoreHandle_t s_send_lock;
static bool s_connected;
static bool s_voice_active;
static tater_state_callback_t s_state_cb;
static tater_play_url_callback_t s_play_url_cb;
static tater_play_tone_callback_t s_play_tone_cb;
static tater_ota_url_callback_t s_ota_url_cb;
static tater_state_t s_current_state = TATER_STATE_DISCONNECTED;
static tater_config_t s_config;
static char s_device_id[48];
static char s_ws_url[192];
static char s_auth_header[160];
static char s_pending_reopen_conversation_id[48];
static uint32_t s_audio_send_logs;
static uint32_t s_audio_send_failures;
static uint32_t s_rx_text_logs;
static bool s_pending_reopen;
static bool s_voice_start_pending;
static bool s_voice_continued_reopen;
static uint32_t s_voice_generation;
static int64_t s_voice_started_us;
static char s_voice_source[24];
static int64_t s_last_link_down_us;
static int64_t s_last_reconnect_attempt_us;
static int64_t s_last_hello_us;
static bool s_hello_acked;
static bool s_playback_return_armed;
static tater_state_t s_playback_return_state = TATER_STATE_IDLE;
static bool s_playback_visual_active;
static int64_t s_playback_visual_started_us;
static bool s_tool_visual_hold;
static bool s_timer_active;
static bool s_timer_ringing;
static char s_timer_id[48];
static char s_timer_label[64];
static int64_t s_timer_deadline_us;
static TaskHandle_t s_timer_alarm_task;
static TaskHandle_t s_timer_monitor_task;
static char s_last_link_down_detail[96];
static int s_last_ws_error_type;
static int s_last_ws_tls_err;
static int s_last_ws_stack_err;
static int s_last_ws_sock_errno;
static int s_last_ws_http_status;
static int s_last_audio_send_result;
static uint32_t s_last_audio_send_samples;
static uint32_t s_audio_send_failure_total;
static bool s_recreate_client_on_reconnect;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static int send_audio_locked(const int16_t *pcm, size_t sample_count, TickType_t timeout);
static void audio_tx_clear_queue(void);
static void audio_tx_task(void *arg);
static void timer_monitor_task(void *arg);
static void continued_reopen_watchdog_task(void *arg);

typedef struct {
    bool initialized;
    esp_reset_reason_t reason;
    bool coredump_present;
    bool coredump_valid;
    size_t coredump_addr;
    size_t coredump_size;
    char coredump_error[32];
    char panic_reason[160];
    char crash_task[17];
    uint32_t crash_pc;
    uint32_t exc_cause;
    uint32_t exc_vaddr;
    char backtrace[180];
} tater_reset_diag_t;

static tater_reset_diag_t s_reset_diag;

typedef struct {
    char conversation_id[sizeof(s_pending_reopen_conversation_id)];
} tater_reopen_args_t;

typedef struct {
    uint32_t generation;
} tater_voice_watchdog_args_t;

#define TATER_WS_RECONNECT_AFTER_MS 7000
#define TATER_WS_RECONNECT_MIN_INTERVAL_MS 7000
#define TATER_WS_HELLO_ACK_TIMEOUT_MS 5000
#define TATER_PLAYBACK_VISUAL_HOLD_MS 30000

#define TATER_AUDIO_PREROLL_SAMPLES (TATER_MIC_SAMPLE_RATE)
#define TATER_AUDIO_TX_QUEUE_CHUNKS 128
#ifndef TATER_AUDIO_TX_BATCH_FRAMES
#define TATER_AUDIO_TX_BATCH_FRAMES 320
#endif
#if TATER_AUDIO_TX_BATCH_FRAMES < TATER_MIC_CHUNK_FRAMES
#error "TATER_AUDIO_TX_BATCH_FRAMES must be >= TATER_MIC_CHUNK_FRAMES"
#endif
#define TATER_AUDIO_TX_BATCH_WAIT_MS 10
#define TATER_AUDIO_TX_SEND_TIMEOUT_MS 3000
#define TATER_AUDIO_TX_LINK_DOWN_FAILURES 3
#define TATER_AUDIO_TX_DRAIN_WAIT_MS 250
#define TATER_AUDIO_TX_CONGESTED_DEPTH 64
#define TATER_AUDIO_TX_RECOVERY_DEPTH 24
#define TATER_AUDIO_TX_SLOW_SEND_MS 1200
#define TATER_CONTINUED_REOPEN_HARD_TIMEOUT_MS 12000

typedef struct {
    uint16_t samples;
    int16_t pcm[TATER_AUDIO_TX_BATCH_FRAMES];
} tater_audio_tx_chunk_t;

static int16_t s_audio_preroll[TATER_AUDIO_PREROLL_SAMPLES];
static size_t s_audio_preroll_start;
static size_t s_audio_preroll_count;
static tater_audio_tx_chunk_t *s_audio_tx_queue;
static size_t s_audio_tx_capacity;
static size_t s_audio_tx_head;
static size_t s_audio_tx_count;
static SemaphoreHandle_t s_audio_tx_lock;
static SemaphoreHandle_t s_audio_tx_has_data;
static TaskHandle_t s_audio_tx_task;
static uint32_t s_audio_tx_high_water;
static uint32_t s_audio_tx_dropped;
static uint32_t s_audio_tx_overruns;
static uint32_t s_audio_tx_send_timeouts;
static uint32_t s_audio_tx_last_send_ms;
static uint32_t s_audio_tx_last_queue_depth;
static int64_t s_status_deferred_log_us;

static double now_seconds(void)
{
    return (double)esp_timer_get_time() / 1000000.0;
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power_glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu_lockup";
    default:
        return "unknown";
    }
}

static const char *xmos_update_state_name(tater_audio_xmos_update_state_t state)
{
    switch (state) {
    case TATER_XMOS_UPDATE_IDLE:
        return "idle";
    case TATER_XMOS_UPDATE_SKIPPED:
        return "skipped";
    case TATER_XMOS_UPDATE_RUNNING:
        return "running";
    case TATER_XMOS_UPDATE_COMPLETE:
        return "complete";
    case TATER_XMOS_UPDATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static void reset_diag_init_once(void)
{
    if (s_reset_diag.initialized) {
        return;
    }

    memset(&s_reset_diag, 0, sizeof(s_reset_diag));
    s_reset_diag.initialized = true;
    s_reset_diag.reason = esp_reset_reason();

    size_t dump_addr = 0;
    size_t dump_size = 0;
    esp_err_t get_err = esp_core_dump_image_get(&dump_addr, &dump_size);
    esp_err_t check_err = esp_core_dump_image_check();
    s_reset_diag.coredump_present = check_err == ESP_OK || (get_err == ESP_OK && dump_size > 0);
    s_reset_diag.coredump_valid = check_err == ESP_OK;
    s_reset_diag.coredump_addr = dump_addr;
    s_reset_diag.coredump_size = dump_size;
    if (s_reset_diag.coredump_present && check_err != ESP_OK && check_err != ESP_ERR_NOT_FOUND) {
        snprintf(s_reset_diag.coredump_error, sizeof(s_reset_diag.coredump_error), "%s", esp_err_to_name(check_err));
    }

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (s_reset_diag.coredump_valid) {
        esp_err_t panic_err = esp_core_dump_get_panic_reason(s_reset_diag.panic_reason, sizeof(s_reset_diag.panic_reason));
        if (panic_err != ESP_OK) {
            s_reset_diag.panic_reason[0] = '\0';
        }

        esp_core_dump_summary_t summary = {0};
        esp_err_t summary_err = esp_core_dump_get_summary(&summary);
        if (summary_err == ESP_OK) {
            snprintf(s_reset_diag.crash_task, sizeof(s_reset_diag.crash_task), "%s", summary.exc_task);
            s_reset_diag.crash_pc = summary.exc_pc;
            s_reset_diag.exc_cause = summary.ex_info.exc_cause;
            s_reset_diag.exc_vaddr = summary.ex_info.exc_vaddr;

            size_t used = 0;
            uint32_t depth = summary.exc_bt_info.depth;
            if (depth > 16) {
                depth = 16;
            }
            for (uint32_t i = 0; i < depth && used < sizeof(s_reset_diag.backtrace); i++) {
                int wrote = snprintf(
                    s_reset_diag.backtrace + used,
                    sizeof(s_reset_diag.backtrace) - used,
                    "%s0x%08lx",
                    used > 0 ? " " : "",
                    (unsigned long)summary.exc_bt_info.bt[i]
                );
                if (wrote <= 0) {
                    break;
                }
                used += (size_t)wrote;
            }
        }
    }
#endif

    ESP_LOGI(
        TAG,
        "reset reason=%s(%d) coredump_present=%d valid=%d size=%u",
        reset_reason_name(s_reset_diag.reason),
        (int)s_reset_diag.reason,
        s_reset_diag.coredump_present,
        s_reset_diag.coredump_valid,
        (unsigned)s_reset_diag.coredump_size
    );
}

static cJSON *reset_diag_json(void)
{
    reset_diag_init_once();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "reason_code", (int)s_reset_diag.reason);
    cJSON_AddStringToObject(root, "reason", reset_reason_name(s_reset_diag.reason));
    cJSON_AddBoolToObject(root, "coredump_present", s_reset_diag.coredump_present);
    cJSON_AddBoolToObject(root, "coredump_valid", s_reset_diag.coredump_valid);
    cJSON_AddNumberToObject(root, "coredump_addr", (double)s_reset_diag.coredump_addr);
    cJSON_AddNumberToObject(root, "coredump_size", (double)s_reset_diag.coredump_size);
    if (s_reset_diag.coredump_error[0]) {
        cJSON_AddStringToObject(root, "coredump_error", s_reset_diag.coredump_error);
    }
    if (s_reset_diag.panic_reason[0]) {
        cJSON_AddStringToObject(root, "panic_reason", s_reset_diag.panic_reason);
    }
    if (s_reset_diag.crash_task[0]) {
        cJSON_AddStringToObject(root, "crash_task", s_reset_diag.crash_task);
        cJSON_AddNumberToObject(root, "crash_pc", s_reset_diag.crash_pc);
        cJSON_AddNumberToObject(root, "exc_cause", s_reset_diag.exc_cause);
        cJSON_AddNumberToObject(root, "exc_vaddr", s_reset_diag.exc_vaddr);
    }
    if (s_reset_diag.backtrace[0]) {
        cJSON_AddStringToObject(root, "backtrace", s_reset_diag.backtrace);
    }
    return root;
}

static void make_id(char *out, size_t out_len)
{
    snprintf(out, out_len, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
}

static bool playback_finished_detail(const char *detail)
{
    return detail
        && (strcmp(detail, "playback return") == 0
            || strcmp(detail, "playback finished") == 0
            || strcmp(detail, "playback stopped") == 0);
}

static void mark_playback_visual_active(void)
{
    s_playback_visual_active = true;
    s_playback_visual_started_us = esp_timer_get_time();
}

static void clear_playback_visual_active(void)
{
    s_playback_visual_active = false;
    s_playback_visual_started_us = 0;
}

static bool playback_visual_holds_state(void)
{
    if (tater_playback_is_playing()) {
        return true;
    }
    if (!s_playback_visual_active || s_playback_visual_started_us <= 0) {
        return false;
    }
    int64_t age_ms = (esp_timer_get_time() - s_playback_visual_started_us) / 1000;
    if (age_ms < 0 || age_ms > TATER_PLAYBACK_VISUAL_HOLD_MS) {
        clear_playback_visual_active();
        return false;
    }
    return true;
}

static bool playback_turn_in_progress(void)
{
    return tater_playback_is_playing() || playback_visual_holds_state();
}

static void emit_state(tater_state_t state, const char *detail)
{
    if (s_timer_ringing && state != TATER_STATE_TIMER && state != TATER_STATE_OTA && state != TATER_STATE_PROVISIONING) {
        ESP_LOGI(TAG, "state=%d detail=%s ignored during timer alarm", (int)state, detail ? detail : "");
        return;
    }

    if (state == TATER_STATE_TIMER) {
        s_tool_visual_hold = false;
    } else if (state == TATER_STATE_TOOL_CALL) {
        s_tool_visual_hold = true;
    } else if ((state == TATER_STATE_IDLE || state == TATER_STATE_DISCONNECTED || state == TATER_STATE_ERROR)
        && playback_visual_holds_state()) {
        if (!playback_finished_detail(detail)) {
            ESP_LOGI(TAG, "state=%d detail=%s ignored during playback", (int)state, detail ? detail : "");
            return;
        }
    } else if (state == TATER_STATE_THINKING || state == TATER_STATE_SPEAKING) {
        if (s_tool_visual_hold) {
            ESP_LOGI(TAG, "state=%d detail=%s ignored during tool visual hold", (int)state, detail ? detail : "");
            return;
        }
    } else {
        s_tool_visual_hold = false;
    }

    s_current_state = state;
    if (s_state_cb) {
        s_state_cb(state, detail);
    }
}

static void mark_link_down(const char *detail)
{
    bool changed = s_connected || s_voice_active;
    s_last_link_down_us = esp_timer_get_time();
    snprintf(s_last_link_down_detail, sizeof(s_last_link_down_detail), "%s", detail ? detail : "disconnected");
    s_connected = false;
    s_hello_acked = false;
    s_voice_active = false;
    s_voice_start_pending = false;
    s_voice_continued_reopen = false;
    s_voice_generation++;
    s_voice_started_us = 0;
    s_voice_source[0] = '\0';
    s_audio_preroll_start = 0;
    s_audio_preroll_count = 0;
    clear_playback_visual_active();
    audio_tx_clear_queue();
    if (changed) {
        emit_state(TATER_STATE_DISCONNECTED, detail ? detail : "disconnected");
    }
}

static bool websocket_ready(void)
{
    return s_client && s_connected && s_hello_acked && esp_websocket_client_is_connected(s_client);
}

static bool websocket_transport_ready(void)
{
    return s_client && s_connected && esp_websocket_client_is_connected(s_client);
}

static void remember_websocket_error(const esp_websocket_event_data_t *data)
{
    if (!data) {
        return;
    }
    s_last_ws_error_type = (int)data->error_handle.error_type;
    s_last_ws_tls_err = (int)data->error_handle.esp_tls_last_esp_err;
    s_last_ws_stack_err = data->error_handle.esp_tls_stack_err;
    s_last_ws_sock_errno = data->error_handle.esp_transport_sock_errno;
    s_last_ws_http_status = data->error_handle.esp_ws_handshake_status_code;
}

static esp_err_t create_websocket_client(void)
{
    esp_websocket_client_config_t cfg = {
        .uri = s_ws_url,
        .task_prio = 8,
        .task_stack = 8192,
        .buffer_size = 4096,
        .ping_interval_sec = 30,
        .pingpong_timeout_sec = 30,
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
        .disable_auto_reconnect = true,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 15000,
        .enable_close_reconnect = false,
        .headers = strlen(s_auth_header) > 0 ? s_auth_header : NULL,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(client);
        return err;
    }
    s_client = client;
    return ESP_OK;
}

static void reconnect_watchdog_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_client || websocket_ready()) {
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        int64_t since_attempt_us = s_last_reconnect_attempt_us > 0 ? now_us - s_last_reconnect_attempt_us : INT64_MAX;
        int64_t down_us = s_last_link_down_us > 0 ? now_us - s_last_link_down_us : now_us;
        int64_t hello_age_us = s_last_hello_us > 0 ? now_us - s_last_hello_us : INT64_MAX;
        const char *reason = "down";

        if (websocket_transport_ready() && !s_hello_acked) {
            if (hello_age_us < (int64_t)TATER_WS_HELLO_ACK_TIMEOUT_MS * 1000
                || since_attempt_us < (int64_t)TATER_WS_RECONNECT_MIN_INTERVAL_MS * 1000) {
                continue;
            }
            reason = "hello_ack_timeout";
            down_us = hello_age_us;
        } else {
            if (down_us < (int64_t)TATER_WS_RECONNECT_AFTER_MS * 1000
                || since_attempt_us < (int64_t)TATER_WS_RECONNECT_MIN_INTERVAL_MS * 1000) {
                continue;
            }
        }

        s_last_reconnect_attempt_us = now_us;
        bool client_connected = esp_websocket_client_is_connected(s_client);
        ESP_LOGW(
            TAG,
            "websocket reconnect watchdog reason=%s down_ms=%lld client_connected=%d hello_acked=%d",
            reason,
            (long long)(down_us / 1000),
            client_connected,
            s_hello_acked
        );

        if (s_send_lock) {
            xSemaphoreTake(s_send_lock, portMAX_DELAY);
        }
        bool recreate_client = s_recreate_client_on_reconnect;
        esp_err_t stop_err = esp_websocket_client_stop(s_client);
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "websocket watchdog stop result=%s", esp_err_to_name(stop_err));
        }
        if (recreate_client) {
            ESP_LOGI(TAG, "websocket watchdog recreating client with updated auth header");
            esp_err_t destroy_err = esp_websocket_client_destroy(s_client);
            if (destroy_err != ESP_OK) {
                ESP_LOGW(TAG, "websocket watchdog destroy result=%s", esp_err_to_name(destroy_err));
            }
            s_client = NULL;
            esp_err_t create_err = create_websocket_client();
            if (create_err != ESP_OK) {
                ESP_LOGE(TAG, "websocket watchdog create result=%s", esp_err_to_name(create_err));
                if (s_send_lock) {
                    xSemaphoreGive(s_send_lock);
                }
                continue;
            }
            s_recreate_client_on_reconnect = false;
        }
        if (s_send_lock) {
            xSemaphoreGive(s_send_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(250));

        if (s_send_lock) {
            xSemaphoreTake(s_send_lock, portMAX_DELAY);
        }
        esp_err_t start_err = esp_websocket_client_start(s_client);
        if (s_send_lock) {
            xSemaphoreGive(s_send_lock);
        }
        if (start_err != ESP_OK) {
            ESP_LOGW(TAG, "websocket watchdog start result=%s", esp_err_to_name(start_err));
        }
    }
}

static esp_err_t audio_tx_init(void)
{
    if (s_audio_tx_queue) {
        return ESP_OK;
    }

    s_audio_tx_lock = xSemaphoreCreateMutex();
    s_audio_tx_has_data = xSemaphoreCreateBinary();
    if (!s_audio_tx_lock || !s_audio_tx_has_data) {
        return ESP_ERR_NO_MEM;
    }

    const size_t bytes = sizeof(tater_audio_tx_chunk_t) * TATER_AUDIO_TX_QUEUE_CHUNKS;
    s_audio_tx_queue = (tater_audio_tx_chunk_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_audio_tx_queue) {
        s_audio_tx_queue = (tater_audio_tx_chunk_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (!s_audio_tx_queue) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_audio_tx_queue, 0, bytes);
    s_audio_tx_capacity = TATER_AUDIO_TX_QUEUE_CHUNKS;
    ESP_LOGI(TAG, "audio tx queue ready chunks=%u bytes=%u", (unsigned)s_audio_tx_capacity, (unsigned)bytes);
    return ESP_OK;
}

static void audio_tx_start_task(void)
{
    if (!s_audio_tx_queue || s_audio_tx_task) {
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(audio_tx_task, "tater_audio_tx", 8192, NULL, 7, &s_audio_tx_task, 0);
    if (ok != pdPASS) {
        s_audio_tx_task = NULL;
        ESP_LOGE(TAG, "audio tx task create failed");
    }
}

static size_t audio_tx_queue_depth(void)
{
    size_t depth = 0;
    if (!s_audio_tx_lock) {
        return 0;
    }
    xSemaphoreTake(s_audio_tx_lock, portMAX_DELAY);
    depth = s_audio_tx_count;
    xSemaphoreGive(s_audio_tx_lock);
    return depth;
}

static void audio_tx_clear_queue(void)
{
    if (!s_audio_tx_lock) {
        return;
    }
    xSemaphoreTake(s_audio_tx_lock, portMAX_DELAY);
    s_audio_tx_head = 0;
    s_audio_tx_count = 0;
    s_audio_tx_last_queue_depth = 0;
    xSemaphoreGive(s_audio_tx_lock);
    if (s_audio_tx_has_data) {
        while (xSemaphoreTake(s_audio_tx_has_data, 0) == pdTRUE) {
        }
    }
}

static void audio_tx_count_dropped(uint32_t dropped)
{
    if (!dropped || !s_audio_tx_lock) {
        return;
    }
    xSemaphoreTake(s_audio_tx_lock, portMAX_DELAY);
    s_audio_tx_dropped += dropped;
    s_audio_tx_overruns += dropped;
    xSemaphoreGive(s_audio_tx_lock);
}

static uint32_t audio_tx_drop_oldest_to_depth(size_t target_depth)
{
    if (!s_audio_tx_lock) {
        return 0;
    }
    uint32_t dropped = 0;
    xSemaphoreTake(s_audio_tx_lock, portMAX_DELAY);
    while (s_audio_tx_count > target_depth) {
        s_audio_tx_head = (s_audio_tx_head + 1) % s_audio_tx_capacity;
        s_audio_tx_count--;
        dropped++;
    }
    if (dropped > 0) {
        s_audio_tx_dropped += dropped;
        s_audio_tx_overruns += dropped;
        s_audio_tx_last_queue_depth = (uint32_t)s_audio_tx_count;
    }
    xSemaphoreGive(s_audio_tx_lock);
    return dropped;
}

static bool audio_tx_wait_drained(TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    while (audio_tx_queue_depth() > 0) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static bool audio_tx_enqueue(const int16_t *pcm, size_t sample_count, const char *source)
{
    if (!s_audio_tx_queue || !s_audio_tx_lock || !s_audio_tx_has_data || !pcm || sample_count == 0) {
        return false;
    }

    size_t offset = 0;
    while (offset < sample_count) {
        size_t chunk_samples = sample_count - offset;
        if (chunk_samples > TATER_AUDIO_TX_BATCH_FRAMES) {
            chunk_samples = TATER_AUDIO_TX_BATCH_FRAMES;
        }

        bool log_drop = false;
        uint32_t dropped_after = 0;
        xSemaphoreTake(s_audio_tx_lock, portMAX_DELAY);
        if (s_audio_tx_count >= s_audio_tx_capacity) {
            s_audio_tx_head = (s_audio_tx_head + 1) % s_audio_tx_capacity;
            s_audio_tx_count--;
            s_audio_tx_dropped++;
            s_audio_tx_overruns++;
            if ((s_audio_tx_overruns == 1) || (s_audio_tx_overruns % 25 == 0)) {
                log_drop = true;
                dropped_after = s_audio_tx_dropped;
            }
        }

        size_t tail = (s_audio_tx_head + s_audio_tx_count) % s_audio_tx_capacity;
        s_audio_tx_queue[tail].samples = (uint16_t)chunk_samples;
        memcpy(s_audio_tx_queue[tail].pcm, pcm + offset, chunk_samples * sizeof(int16_t));
        s_audio_tx_count++;
        s_audio_tx_last_queue_depth = (uint32_t)s_audio_tx_count;
        if (s_audio_tx_count > s_audio_tx_high_water) {
            s_audio_tx_high_water = (uint32_t)s_audio_tx_count;
        }
        xSemaphoreGive(s_audio_tx_lock);
        xSemaphoreGive(s_audio_tx_has_data);
        if (log_drop) {
            ESP_LOGW(
                TAG,
                "audio tx queue full; dropping oldest source=%s dropped=%u",
                source ? source : "-",
                (unsigned)dropped_after
            );
        }

        offset += chunk_samples;
    }

    return true;
}

static bool audio_tx_pop(tater_audio_tx_chunk_t *out, size_t *depth_after)
{
    if (!s_audio_tx_queue || !s_audio_tx_lock || !out) {
        return false;
    }

    bool ok = false;
    xSemaphoreTake(s_audio_tx_lock, portMAX_DELAY);
    if (s_audio_tx_count > 0) {
        *out = s_audio_tx_queue[s_audio_tx_head];
        s_audio_tx_head = (s_audio_tx_head + 1) % s_audio_tx_capacity;
        s_audio_tx_count--;
        s_audio_tx_last_queue_depth = (uint32_t)s_audio_tx_count;
        if (depth_after) {
            *depth_after = s_audio_tx_count;
        }
        ok = true;
    } else if (depth_after) {
        *depth_after = 0;
    }
    xSemaphoreGive(s_audio_tx_lock);
    return ok;
}

static void record_audio_send_result(int sent, size_t sample_count, uint32_t elapsed_ms, size_t queue_depth)
{
    int expected = (int)(sample_count * sizeof(int16_t));
    bool failed = sent < expected;
    s_last_audio_send_result = sent;
    s_last_audio_send_samples = (uint32_t)sample_count;
    s_audio_tx_last_send_ms = elapsed_ms;
    s_audio_tx_last_queue_depth = (uint32_t)queue_depth;

    if (s_audio_send_logs < 3 || failed || elapsed_ms > TATER_AUDIO_TX_SEND_TIMEOUT_MS) {
        ESP_LOGI(
            TAG,
            "audio bin send samples=%u bytes=%u result=%d elapsed_ms=%u queue=%u",
            (unsigned)sample_count,
            (unsigned)(sample_count * sizeof(int16_t)),
            sent,
            (unsigned)elapsed_ms,
            (unsigned)queue_depth
        );
        s_audio_send_logs++;
    }

    if (failed) {
        s_audio_send_failures++;
        s_audio_send_failure_total++;
        if (elapsed_ms >= TATER_AUDIO_TX_SEND_TIMEOUT_MS) {
            s_audio_tx_send_timeouts++;
        }
    } else {
        s_audio_send_failures = 0;
    }
}

static void audio_tx_task(void *arg)
{
    (void)arg;
    tater_audio_tx_chunk_t chunk;
    tater_audio_tx_chunk_t next;

    while (true) {
        if (!audio_tx_pop(&chunk, NULL)) {
            xSemaphoreTake(s_audio_tx_has_data, portMAX_DELAY);
            continue;
        }

        if (chunk.samples == 0) {
            continue;
        }
        if (!websocket_ready() || !s_voice_active || s_voice_start_pending) {
            audio_tx_clear_queue();
            continue;
        }

        size_t pending_depth = audio_tx_queue_depth();
        if (pending_depth >= TATER_AUDIO_TX_CONGESTED_DEPTH) {
            audio_tx_count_dropped(1);
            uint32_t dropped = audio_tx_drop_oldest_to_depth(TATER_AUDIO_TX_RECOVERY_DEPTH);
            ESP_LOGW(
                TAG,
                "audio tx congested; dropped stale chunks current=1 queued=%u depth_before=%u",
                (unsigned)dropped,
                (unsigned)pending_depth
            );
            continue;
        }

        bool waited_for_batch = false;
        size_t depth_after = audio_tx_queue_depth();
        TickType_t batch_start = xTaskGetTickCount();
        while (chunk.samples < TATER_AUDIO_TX_BATCH_FRAMES && websocket_ready() && s_voice_active && !s_voice_start_pending) {
            if (!audio_tx_pop(&next, &depth_after)) {
                TickType_t elapsed = xTaskGetTickCount() - batch_start;
                TickType_t max_wait = pdMS_TO_TICKS(TATER_AUDIO_TX_BATCH_WAIT_MS);
                if (waited_for_batch && elapsed >= max_wait) {
                    break;
                }
                waited_for_batch = true;
                TickType_t remaining = elapsed >= max_wait ? 0 : (max_wait - elapsed);
                TickType_t wait_ticks = remaining < pdMS_TO_TICKS(12) ? remaining : pdMS_TO_TICKS(12);
                if (wait_ticks == 0 || xSemaphoreTake(s_audio_tx_has_data, wait_ticks) != pdTRUE) {
                    break;
                }
                continue;
            }
            if (next.samples == 0) {
                continue;
            }
            size_t space = TATER_AUDIO_TX_BATCH_FRAMES - chunk.samples;
            size_t copy_samples = next.samples < space ? next.samples : space;
            memcpy(chunk.pcm + chunk.samples, next.pcm, copy_samples * sizeof(int16_t));
            chunk.samples = (uint16_t)(chunk.samples + copy_samples);
            if (copy_samples < next.samples) {
                ESP_LOGW(TAG, "audio tx batch overflow; dropping tail samples=%u", (unsigned)(next.samples - copy_samples));
                break;
            }
        }

        int sent = -1;
        int64_t start_us = esp_timer_get_time();
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
        if (websocket_ready() && s_voice_active && !s_voice_start_pending) {
            sent = send_audio_locked(chunk.pcm, chunk.samples, pdMS_TO_TICKS(TATER_AUDIO_TX_SEND_TIMEOUT_MS));
        }
        xSemaphoreGive(s_send_lock);
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        record_audio_send_result(sent, chunk.samples, elapsed_ms, depth_after);

        if (sent >= (int)(chunk.samples * sizeof(int16_t))
            && elapsed_ms >= TATER_AUDIO_TX_SLOW_SEND_MS
            && depth_after >= TATER_AUDIO_TX_CONGESTED_DEPTH) {
            uint32_t dropped = audio_tx_drop_oldest_to_depth(TATER_AUDIO_TX_RECOVERY_DEPTH);
            if (dropped > 0) {
                ESP_LOGW(
                    TAG,
                    "audio tx slow send; dropped stale backlog elapsed_ms=%u dropped=%u depth_before=%u",
                    (unsigned)elapsed_ms,
                    (unsigned)dropped,
                    (unsigned)depth_after
                );
            }
        }

        if (sent < (int)(chunk.samples * sizeof(int16_t))
            && !websocket_transport_ready()
            && s_audio_send_failures >= TATER_AUDIO_TX_LINK_DOWN_FAILURES) {
            mark_link_down("websocket audio transport lost");
        }
    }
}

static void clear_audio_preroll_locked(void)
{
    s_audio_preroll_start = 0;
    s_audio_preroll_count = 0;
}

static void clear_voice_capture_state(void)
{
    if (s_send_lock) {
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
        s_voice_active = false;
        s_voice_start_pending = false;
        s_voice_continued_reopen = false;
        s_voice_generation++;
        s_voice_started_us = 0;
        s_voice_source[0] = '\0';
        clear_audio_preroll_locked();
        audio_tx_clear_queue();
        xSemaphoreGive(s_send_lock);
    } else {
        s_voice_active = false;
        s_voice_start_pending = false;
        s_voice_continued_reopen = false;
        s_voice_generation++;
        s_voice_started_us = 0;
        s_voice_source[0] = '\0';
        s_audio_preroll_start = 0;
        s_audio_preroll_count = 0;
        audio_tx_clear_queue();
    }
}

static void buffer_audio_preroll_locked(const int16_t *pcm, size_t sample_count)
{
    if (!pcm || sample_count == 0) {
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        if (s_audio_preroll_count < TATER_AUDIO_PREROLL_SAMPLES) {
            size_t index = (s_audio_preroll_start + s_audio_preroll_count) % TATER_AUDIO_PREROLL_SAMPLES;
            s_audio_preroll[index] = pcm[i];
            s_audio_preroll_count++;
        } else {
            s_audio_preroll[s_audio_preroll_start] = pcm[i];
            s_audio_preroll_start = (s_audio_preroll_start + 1) % TATER_AUDIO_PREROLL_SAMPLES;
        }
    }
}

static int send_audio_locked(const int16_t *pcm, size_t sample_count, TickType_t timeout)
{
    return esp_websocket_client_send_bin(s_client, (const char *)pcm, sample_count * sizeof(int16_t), timeout);
}

static void flush_audio_preroll_locked(void)
{
    if (!websocket_ready() || !s_voice_active || s_audio_preroll_count == 0) {
        clear_audio_preroll_locked();
        return;
    }

    int16_t chunk[TATER_AUDIO_TX_BATCH_FRAMES];
    size_t flushed = 0;
    while (s_audio_preroll_count > 0) {
        size_t chunk_samples = s_audio_preroll_count < TATER_AUDIO_TX_BATCH_FRAMES ? s_audio_preroll_count : TATER_AUDIO_TX_BATCH_FRAMES;
        for (size_t i = 0; i < chunk_samples; i++) {
            chunk[i] = s_audio_preroll[(s_audio_preroll_start + i) % TATER_AUDIO_PREROLL_SAMPLES];
        }
        if (!audio_tx_enqueue(chunk, chunk_samples, "preroll")) {
            ESP_LOGW(TAG, "audio preroll queue failed samples=%u", (unsigned)chunk_samples);
            clear_audio_preroll_locked();
            return;
        }
        s_audio_preroll_start = (s_audio_preroll_start + chunk_samples) % TATER_AUDIO_PREROLL_SAMPLES;
        s_audio_preroll_count -= chunk_samples;
        flushed += chunk_samples;
    }
    if (flushed > 0) {
        ESP_LOGI(TAG, "audio preroll queued samples=%u", (unsigned)flushed);
    }
}

static const char *xmos_prerelease_name(uint8_t prerelease)
{
    switch (prerelease) {
    case 1:
        return "alpha";
    case 2:
        return "beta";
    case 3:
        return "rc";
    case 4:
        return "dev";
    default:
        return "";
    }
}

static void format_xmos_version(
    char *out,
    size_t out_len,
    uint8_t major,
    uint8_t minor,
    uint8_t patch,
    uint8_t prerelease,
    uint8_t counter
)
{
    if (!out || out_len == 0) {
        return;
    }
    const char *pre = xmos_prerelease_name(prerelease);
    if (pre[0] && counter) {
        snprintf(out, out_len, "%u.%u.%u-%s.%u", major, minor, patch, pre, counter);
    } else if (pre[0]) {
        snprintf(out, out_len, "%u.%u.%u-%s", major, minor, patch, pre);
    } else {
        snprintf(out, out_len, "%u.%u.%u", major, minor, patch);
    }
}

static void build_device_id(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(
        s_device_id,
        sizeof(s_device_id),
        "%s-%02x%02x%02x",
        CONFIG_TATER_DEVICE_ID_PREFIX,
        mac[3],
        mac[4],
        mac[5]
    );
}

static void build_ws_url(void)
{
    char base[144];
    strlcpy(base, s_config.server_url, sizeof(base));
    if (strstr(base, NATIVE_WS_PATH) != NULL) {
        strlcpy(s_ws_url, base, sizeof(s_ws_url));
        return;
    }

    if (strncmp(base, "http://", 7) == 0) {
        memmove(base + 2, base + 4, strlen(base + 4) + 1);
        base[0] = 'w';
        base[1] = 's';
    } else if (strncmp(base, "https://", 8) == 0) {
        memmove(base + 3, base + 5, strlen(base + 5) + 1);
        base[0] = 'w';
        base[1] = 's';
        base[2] = 's';
    } else if (strncmp(base, "ws://", 5) != 0 && strncmp(base, "wss://", 6) != 0) {
        char tmp[144];
        strlcpy(tmp, base, sizeof(tmp));
        snprintf(base, sizeof(base), "ws://%s", tmp);
    }

    size_t len = strlen(base);
    while (len > 0 && base[len - 1] == '/') {
        base[len - 1] = '\0';
        len--;
    }
    snprintf(s_ws_url, sizeof(s_ws_url), "%s%s", base, NATIVE_WS_PATH);
}

static bool should_log_send(const char *type, int sent)
{
    if (sent < 0) {
        return true;
    }
    return strcmp(type, "hello") == 0
        || strcmp(type, "status") == 0
        || strcmp(type, "timer.event") == 0
        || strcmp(type, "voice.start") == 0
        || strcmp(type, "voice.stop") == 0;
}

static int send_json(cJSON *root)
{
    if (!root) {
        return -1;
    }
    const cJSON *type_item = cJSON_GetObjectItem(root, "type");
    char type[32] = {0};
    strlcpy(type, cJSON_IsString(type_item) ? type_item->valuestring : "", sizeof(type));
    bool is_hello = strcmp(type, "hello") == 0;
    if (!(is_hello ? websocket_transport_ready() : websocket_ready())) {
        cJSON_Delete(root);
        return -1;
    }

    char *wire = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!wire) {
        return -1;
    }

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    int sent = esp_websocket_client_send_text(s_client, wire, strlen(wire), pdMS_TO_TICKS(1000));
    xSemaphoreGive(s_send_lock);
    if (should_log_send(type, sent)) {
        ESP_LOGI(TAG, "json send type=%s bytes=%u result=%d", type, (unsigned)strlen(wire), sent);
    }
    if (sent < 0) {
        mark_link_down("websocket send failed");
    }
    cJSON_free(wire);
    return sent;
}

static cJSON *new_envelope(const char *type)
{
    char id[24];
    make_id(id, sizeof(id));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "ts", now_seconds());
    cJSON_AddItemToObject(root, "payload", cJSON_CreateObject());
    return root;
}

static uint32_t timer_payload_duration_ms(const cJSON *payload, const char *ms_key, const char *s_key, uint32_t fallback_ms)
{
    const cJSON *ms_item = cJSON_GetObjectItem(payload, ms_key);
    if (cJSON_IsNumber(ms_item) && ms_item->valuedouble > 0) {
        return (uint32_t)ms_item->valuedouble;
    }
    const cJSON *s_item = cJSON_GetObjectItem(payload, s_key);
    if (cJSON_IsNumber(s_item) && s_item->valuedouble > 0) {
        return (uint32_t)(s_item->valuedouble * 1000.0);
    }
    return fallback_ms;
}

static void timer_copy_identity(const cJSON *payload)
{
    const cJSON *id_item = cJSON_GetObjectItem(payload, "id");
    const cJSON *label_item = cJSON_GetObjectItem(payload, "label");
    if (cJSON_IsString(id_item) && id_item->valuestring && id_item->valuestring[0]) {
        strlcpy(s_timer_id, id_item->valuestring, sizeof(s_timer_id));
    }
    if (cJSON_IsString(label_item) && label_item->valuestring) {
        strlcpy(s_timer_label, label_item->valuestring, sizeof(s_timer_label));
    }
}

static void timer_emit_event(const char *event)
{
    cJSON *root = new_envelope("timer.event");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "event", event ? event : "");
    cJSON_AddStringToObject(payload, "id", s_timer_id);
    cJSON_AddStringToObject(payload, "label", s_timer_label);
    cJSON_AddBoolToObject(payload, "active", s_timer_active);
    cJSON_AddBoolToObject(payload, "ringing", s_timer_ringing);
    if (s_timer_active && s_timer_deadline_us > 0) {
        int64_t remaining_ms = (s_timer_deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms < 0) {
            remaining_ms = 0;
        }
        cJSON_AddNumberToObject(payload, "remaining_ms", remaining_ms);
    }
    send_json(root);
}

static void timer_alarm_task(void *arg)
{
    (void)arg;
    while (s_timer_ringing) {
        emit_state(TATER_STATE_TIMER, "timer ringing");
        if (!tater_playback_is_playing()) {
            esp_err_t err = tater_playback_play_tone_local(880, 420, 80);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "timer alarm tone failed: %s", esp_err_to_name(err));
            }
        }
        for (int i = 0; i < 18 && s_timer_ringing; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    s_timer_alarm_task = NULL;
    vTaskDelete(NULL);
}

static void timer_start_alarm_task(void)
{
    if (s_timer_alarm_task) {
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(timer_alarm_task, "tater_timer_alarm", 4096, NULL, 5, &s_timer_alarm_task, 1);
    if (ok != pdPASS) {
        s_timer_alarm_task = NULL;
        ESP_LOGE(TAG, "timer alarm task create failed");
    }
}

static void timer_ring(const char *detail, bool notify_expired)
{
    s_timer_active = true;
    s_timer_ringing = true;
    s_timer_deadline_us = 0;
    emit_state(TATER_STATE_TIMER, detail ? detail : "timer");
    if (notify_expired) {
        timer_emit_event("expired");
    }
    timer_start_alarm_task();
}

static void timer_clear_local(bool notify, const char *event)
{
    bool was_active = s_timer_active;
    bool was_ringing = s_timer_ringing;
    if (notify && was_active) {
        timer_emit_event(event ? event : "stopped");
    }
    s_timer_active = false;
    s_timer_ringing = false;
    s_timer_deadline_us = 0;
    s_timer_id[0] = '\0';
    s_timer_label[0] = '\0';
    if (was_ringing && tater_playback_is_playing()) {
        tater_playback_stop();
    }
    if (s_current_state == TATER_STATE_TIMER) {
        emit_state(websocket_ready() ? TATER_STATE_IDLE : TATER_STATE_DISCONNECTED, "timer cleared");
    }
}

static void timer_arm_from_payload(const cJSON *payload)
{
    uint32_t remaining_ms = timer_payload_duration_ms(payload, "remaining_ms", "remaining_s", 0);
    uint32_t duration_ms = timer_payload_duration_ms(payload, "duration_ms", "duration_s", 0);
    uint32_t arm_ms = remaining_ms > 0 ? remaining_ms : duration_ms;
    if (arm_ms == 0) {
        timer_ring("timer alarm", false);
        return;
    }
    bool was_ringing = s_timer_ringing;
    timer_copy_identity(payload);
    s_timer_active = true;
    s_timer_ringing = false;
    s_timer_deadline_us = esp_timer_get_time() + ((int64_t)arm_ms * 1000LL);
    if (was_ringing && tater_playback_is_playing()) {
        tater_playback_stop();
    }
    emit_state(TATER_STATE_TIMER, "timer armed");
    ESP_LOGI(TAG, "timer armed id=%s remaining_ms=%lu", s_timer_id, (unsigned long)arm_ms);
}

static void timer_alarm_from_payload(const cJSON *payload)
{
    timer_copy_identity(payload);
    ESP_LOGW(TAG, "timer alarm id=%s", s_timer_id);
    timer_ring("timer alarm", false);
}

static void timer_monitor_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_timer_active && !s_timer_ringing && s_timer_deadline_us > 0 && esp_timer_get_time() >= s_timer_deadline_us) {
            ESP_LOGW(TAG, "timer expired locally id=%s", s_timer_id);
            timer_ring("timer expired locally", true);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static bool json_truthy(const cJSON *item)
{
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return item->valuedouble != 0.0;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        return strcmp(item->valuestring, "1") == 0
            || strcasecmp(item->valuestring, "true") == 0
            || strcasecmp(item->valuestring, "yes") == 0
            || strcasecmp(item->valuestring, "on") == 0
            || strcasecmp(item->valuestring, "enabled") == 0;
    }
    return false;
}

static void send_hello(void)
{
    s_hello_acked = false;
    s_last_hello_us = esp_timer_get_time();
    cJSON *root = new_envelope("hello");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "device_id", s_device_id);
    cJSON_AddStringToObject(payload, "device_name", s_config.device_name);
    cJSON_AddStringToObject(payload, "board", TATER_BOARD_ID);
    cJSON_AddStringToObject(payload, "firmware_version", TATER_FIRMWARE_VERSION);
    cJSON_AddStringToObject(payload, "room", s_config.room);
    cJSON_AddItemToObject(payload, "reset", reset_diag_json());

    cJSON *caps = cJSON_CreateObject();
    cJSON_AddBoolToObject(caps, "microphone", true);
    cJSON_AddBoolToObject(caps, "speaker", true);
    cJSON_AddBoolToObject(caps, "led_ring", true);
    cJSON_AddBoolToObject(caps, "display", TATER_BOARD_S3_BOX);
    cJSON_AddBoolToObject(caps, "buttons", true);
    cJSON_AddBoolToObject(caps, "touch", false);
    cJSON_AddBoolToObject(caps, "line_out", TATER_CAP_LINE_OUT);
    cJSON_AddBoolToObject(caps, "local_wake", true);
    cJSON_AddBoolToObject(caps, "live_settings", true);
    cJSON_AddBoolToObject(caps, "setup_mode", true);
    cJSON_AddBoolToObject(caps, "continued_chat_reopen", true);
    cJSON_AddBoolToObject(caps, "barge_in", true);
    cJSON_AddBoolToObject(caps, "tool_call_mode", true);
    cJSON_AddBoolToObject(caps, "timers", true);
    cJSON_AddBoolToObject(caps, "ota", true);
    cJSON_AddBoolToObject(caps, "xmos", TATER_CAP_XMOS);
    cJSON_AddBoolToObject(caps, "aec", true);
    cJSON_AddItemToObject(payload, "capabilities", caps);
    send_json(root);
}

static tater_state_t parse_state(const char *state)
{
    if (!state) {
        return TATER_STATE_IDLE;
    }
    if (strcmp(state, "listening") == 0) {
        return TATER_STATE_LISTENING;
    }
    if (strcmp(state, "thinking") == 0) {
        return TATER_STATE_THINKING;
    }
    if (strcmp(state, "speaking") == 0) {
        return TATER_STATE_SPEAKING;
    }
    if (strcmp(state, "tool_call") == 0 || strcmp(state, "tool") == 0 || strcmp(state, "tool_running") == 0) {
        return TATER_STATE_TOOL_CALL;
    }
    if (strcmp(state, "timer") == 0 || strcmp(state, "timer_ringing") == 0 || strcmp(state, "ringing") == 0) {
        return TATER_STATE_TIMER;
    }
    if (strcmp(state, "ota") == 0 || strcmp(state, "updating") == 0) {
        return TATER_STATE_OTA;
    }
    if (strcmp(state, "provisioning") == 0 || strcmp(state, "pairing") == 0) {
        return TATER_STATE_PROVISIONING;
    }
    if (strcmp(state, "error") == 0) {
        return TATER_STATE_ERROR;
    }
    return TATER_STATE_IDLE;
}

static const char *state_name(tater_state_t state)
{
    switch (state) {
    case TATER_STATE_DISCONNECTED:
        return "disconnected";
    case TATER_STATE_IDLE:
        return "idle";
    case TATER_STATE_PROVISIONING:
        return "provisioning";
    case TATER_STATE_LISTENING:
        return "listening";
    case TATER_STATE_THINKING:
        return "thinking";
    case TATER_STATE_SPEAKING:
        return "speaking";
    case TATER_STATE_TOOL_CALL:
        return "tool_call";
    case TATER_STATE_TIMER:
        return "timer";
    case TATER_STATE_OTA:
        return "ota";
    case TATER_STATE_ERROR:
        return "error";
    default:
        return "idle";
    }
}

static void handle_text_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        return;
    }
    const cJSON *type_item = cJSON_GetObjectItem(root, "type");
    const cJSON *payload = cJSON_GetObjectItem(root, "payload");
    const char *type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    if (s_rx_text_logs < 8 || strcmp(type, "error") == 0) {
        ESP_LOGI(TAG, "json recv type=%s bytes=%d", type, len);
        s_rx_text_logs++;
    }

    if (strcmp(type, "hello.ack") == 0 && cJSON_IsObject(payload)) {
        s_hello_acked = true;
        const cJSON *device_token_item = cJSON_GetObjectItem(payload, "device_token");
        if (cJSON_IsString(device_token_item) && device_token_item->valuestring && device_token_item->valuestring[0]) {
            const char *device_token = device_token_item->valuestring;
            if (strcmp(s_config.token, device_token) != 0) {
                esp_err_t save_err = tater_config_save_token(device_token);
                if (save_err == ESP_OK) {
                    strlcpy(s_config.token, device_token, sizeof(s_config.token));
                    snprintf(s_auth_header, sizeof(s_auth_header), "X-Tater-Token: %s\r\n", s_config.token);
                    s_recreate_client_on_reconnect = true;
                    ESP_LOGI(TAG, "paired with Tater; saved device credential and queued websocket auth refresh");
                } else {
                    ESP_LOGE(TAG, "device credential save failed: %s", esp_err_to_name(save_err));
                    emit_state(TATER_STATE_ERROR, "credential save failed");
                }
            }
        }
    } else if (strcmp(type, "state") == 0 && cJSON_IsObject(payload)) {
        const cJSON *state_item = cJSON_GetObjectItem(payload, "state");
        const char *state = cJSON_IsString(state_item) ? state_item->valuestring : "idle";
        emit_state(parse_state(state), state);
    } else if (strcmp(type, "voice.start.ack") == 0 && cJSON_IsObject(payload)) {
        const cJSON *ok = cJSON_GetObjectItem(payload, "ok");
        if (cJSON_IsBool(ok) && !cJSON_IsTrue(ok)) {
            clear_voice_capture_state();
            emit_state(TATER_STATE_ERROR, "voice.start rejected");
        } else {
            xSemaphoreTake(s_send_lock, portMAX_DELAY);
            if (s_voice_active && s_voice_start_pending) {
                s_voice_start_pending = false;
                flush_audio_preroll_locked();
            }
            xSemaphoreGive(s_send_lock);
        }
    } else if (strcmp(type, "voice.event") == 0 && cJSON_IsObject(payload)) {
        const cJSON *event_item = cJSON_GetObjectItem(payload, "event");
        const cJSON *data = cJSON_GetObjectItem(payload, "data");
        const char *event = cJSON_IsString(event_item) ? event_item->valuestring : "";
        ESP_LOGI(TAG, "voice.event=%s", event);
        if (strcmp(event, "STT_VAD_END") == 0) {
            clear_voice_capture_state();
            emit_state(TATER_STATE_THINKING, "server vad end");
            ESP_LOGI(TAG, "mic stream closed by server VAD");
        } else if (strcmp(event, "INTENT_END") == 0 && cJSON_IsObject(data)) {
            const cJSON *continue_item = cJSON_GetObjectItem(data, "continue_conversation");
            const cJSON *conversation_item = cJSON_GetObjectItem(data, "conversation_id");
            bool continue_conversation = json_truthy(continue_item);
            if (continue_conversation && cJSON_IsString(conversation_item) && conversation_item->valuestring[0]) {
                snprintf(s_pending_reopen_conversation_id, sizeof(s_pending_reopen_conversation_id), "%s", conversation_item->valuestring);
                s_pending_reopen = true;
                ESP_LOGI(TAG, "continued chat reopen armed conversation_id=%s", s_pending_reopen_conversation_id);
            } else {
                s_pending_reopen = false;
                s_pending_reopen_conversation_id[0] = '\0';
            }
        } else if (strcmp(event, "RUN_END") == 0) {
            clear_voice_capture_state();
            bool playback_active = playback_turn_in_progress();
            if (s_pending_reopen && !playback_active) {
                ESP_LOGI(TAG, "continued chat reopen cleared; run ended without active playback");
                s_pending_reopen = false;
                s_pending_reopen_conversation_id[0] = '\0';
            }
            if (!playback_active) {
                emit_state(TATER_STATE_IDLE, "run end");
            }
        } else if (strcmp(event, "ERROR") == 0) {
            clear_voice_capture_state();
            s_pending_reopen = false;
            s_pending_reopen_conversation_id[0] = '\0';
            emit_state(TATER_STATE_ERROR, "voice error");
        }
    } else if (strcmp(type, "play.url") == 0 && cJSON_IsObject(payload)) {
        const cJSON *url_item = cJSON_GetObjectItem(payload, "url");
        const cJSON *tts_kind_item = cJSON_GetObjectItem(payload, "tts_kind");
        const cJSON *visual_mode_item = cJSON_GetObjectItem(payload, "visual_mode");
        const cJSON *state_after_item = cJSON_GetObjectItem(payload, "state_after");
        const char *tts_kind = cJSON_IsString(tts_kind_item) ? tts_kind_item->valuestring : "";
        const char *visual_mode = cJSON_IsString(visual_mode_item) ? visual_mode_item->valuestring : "";
        const char *state_after = cJSON_IsString(state_after_item) ? state_after_item->valuestring : "";
        bool tool_playback = strcmp(tts_kind, "tool") == 0
            || strcmp(tts_kind, "tool_progress") == 0
            || strcmp(visual_mode, "tool_call") == 0
            || strcmp(state_after, "tool_call") == 0;
        tater_state_t visual_state = tool_playback ? TATER_STATE_TOOL_CALL : TATER_STATE_SPEAKING;
        if (state_after && state_after[0]) {
            s_playback_return_state = parse_state(state_after);
            s_playback_return_armed = true;
        } else if (tool_playback) {
            s_playback_return_state = TATER_STATE_TOOL_CALL;
            s_playback_return_armed = true;
        } else {
            s_playback_return_armed = false;
            s_playback_return_state = TATER_STATE_IDLE;
        }
        if (cJSON_IsString(url_item) && s_play_url_cb) {
            ESP_LOGI(
                TAG,
                "play.url kind=%s visual=%s state_after=%s",
                tts_kind && tts_kind[0] ? tts_kind : "-",
                visual_mode && visual_mode[0] ? visual_mode : "-",
                state_after && state_after[0] ? state_after : "-"
            );
            if (!tool_playback) {
                s_tool_visual_hold = false;
            }
            mark_playback_visual_active();
            emit_state(visual_state, tool_playback ? "tool playback" : "playback");
            s_play_url_cb(url_item->valuestring, visual_state);
        }
    } else if (strcmp(type, "play.tone") == 0 && cJSON_IsObject(payload)) {
        const cJSON *frequency_item = cJSON_GetObjectItem(payload, "frequency_hz");
        const cJSON *duration_item = cJSON_GetObjectItem(payload, "duration_ms");
        const cJSON *volume_item = cJSON_GetObjectItem(payload, "volume_percent");
        uint32_t frequency_hz = cJSON_IsNumber(frequency_item) ? (uint32_t)frequency_item->valuedouble : 1000;
        uint32_t duration_ms = cJSON_IsNumber(duration_item) ? (uint32_t)duration_item->valuedouble : 2000;
        uint8_t volume_percent = cJSON_IsNumber(volume_item) ? (uint8_t)volume_item->valuedouble : 60;
        if (s_play_tone_cb) {
            s_play_tone_cb(frequency_hz, duration_ms, volume_percent);
        }
    } else if (strcmp(type, "timer.arm") == 0 && cJSON_IsObject(payload)) {
        timer_arm_from_payload(payload);
    } else if (strcmp(type, "timer.alarm") == 0 && cJSON_IsObject(payload)) {
        timer_alarm_from_payload(payload);
    } else if (strcmp(type, "timer.clear") == 0) {
        timer_clear_local(false, "cleared");
    } else if (strcmp(type, "ota.url") == 0 && cJSON_IsObject(payload)) {
        const cJSON *url_item = cJSON_GetObjectItem(payload, "url");
        if (cJSON_IsString(url_item) && s_ota_url_cb) {
            s_ota_url_cb(url_item->valuestring);
        }
    } else if ((strcmp(type, "setup.reset") == 0 || strcmp(type, "provisioning.reset") == 0) && cJSON_IsObject(payload)) {
        ESP_LOGW(TAG, "setup reset requested by server; clearing provisioning");
        tater_protocol_send_log("warn", "Setup reset requested by Tater; clearing provisioning and rebooting into setup mode.");
        tater_playback_stop();
        emit_state(TATER_STATE_PROVISIONING, "setup reset");
        ESP_ERROR_CHECK_WITHOUT_ABORT(tater_config_clear());
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (strcmp(type, "settings") == 0 && cJSON_IsObject(payload)) {
        tater_live_settings_apply_json(payload);
    } else if (strcmp(type, "error") == 0) {
        clear_voice_capture_state();
        emit_state(TATER_STATE_ERROR, "server error");
    }

    cJSON_Delete(root);
}

static const char *websocket_error_type_name(esp_websocket_error_type_t type)
{
    switch (type) {
    case WEBSOCKET_ERROR_TYPE_NONE:
        return "none";
    case WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT:
        return "tcp_transport";
    case WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT:
        return "pong_timeout";
    case WEBSOCKET_ERROR_TYPE_HANDSHAKE:
        return "handshake";
    case WEBSOCKET_ERROR_TYPE_SERVER_CLOSE:
        return "server_close";
    default:
        return "unknown";
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        s_hello_acked = false;
        s_last_reconnect_attempt_us = 0;
        clear_voice_capture_state();
        s_rx_text_logs = 0;
        ESP_LOGI(TAG, "connected %s", s_ws_url);
        send_hello();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        remember_websocket_error(data);
        mark_link_down("disconnected");
        ESP_LOGW(
            TAG,
            "disconnected error=%s tls=%s stack=%d sock_errno=%d http=%d",
            websocket_error_type_name(data->error_handle.error_type),
            esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
            data->error_handle.esp_tls_stack_err,
            data->error_handle.esp_transport_sock_errno,
            data->error_handle.esp_ws_handshake_status_code
        );
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGD(
            TAG,
            "websocket data op=0x%02x len=%d payload=%d offset=%d fin=%d",
            data->op_code,
            data->data_len,
            data->payload_len,
            data->payload_offset,
            data->fin
        );
        if (data->op_code == 0x1 && data->data_ptr && data->data_len > 0) {
            handle_text_message(data->data_ptr, data->data_len);
        } else if (data->op_code == 0x08) {
            mark_link_down("server close frame");
            ESP_LOGW(TAG, "websocket close frame len=%d", data->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        remember_websocket_error(data);
        mark_link_down("websocket error");
        ESP_LOGE(
            TAG,
            "websocket error=%s tls=%s stack=%d sock_errno=%d http=%d",
            websocket_error_type_name(data->error_handle.error_type),
            esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
            data->error_handle.esp_tls_stack_err,
            data->error_handle.esp_transport_sock_errno,
            data->error_handle.esp_ws_handshake_status_code
        );
        emit_state(TATER_STATE_ERROR, "websocket error");
        break;
    case WEBSOCKET_EVENT_CLOSED:
        remember_websocket_error(data);
        mark_link_down("websocket closed");
        ESP_LOGW(TAG, "websocket closed cleanly");
        break;
    default:
        break;
    }
}

void tater_protocol_init(
    const tater_config_t *config,
    tater_state_callback_t state_cb,
    tater_play_url_callback_t play_url_cb,
    tater_play_tone_callback_t play_tone_cb,
    tater_ota_url_callback_t ota_url_cb
)
{
    if (config) {
        s_config = *config;
    } else {
        tater_config_defaults(&s_config);
    }
    s_state_cb = state_cb;
    s_play_url_cb = play_url_cb;
    s_play_tone_cb = play_tone_cb;
    s_ota_url_cb = ota_url_cb;
    tater_live_settings_init_defaults();
    s_send_lock = xSemaphoreCreateMutex();
    esp_err_t audio_tx_err = audio_tx_init();
    if (audio_tx_err != ESP_OK) {
        ESP_LOGE(TAG, "audio tx queue init failed: %s", esp_err_to_name(audio_tx_err));
    }
    build_device_id();
    build_ws_url();
    if (strlen(s_config.token) > 0) {
        snprintf(s_auth_header, sizeof(s_auth_header), "X-Tater-Token: %s\r\n", s_config.token);
    }
}

void tater_protocol_start(void)
{
    ESP_ERROR_CHECK(create_websocket_client());
    ESP_ERROR_CHECK(esp_websocket_client_start(s_client));
    audio_tx_start_task();
    BaseType_t task_ok = xTaskCreate(reconnect_watchdog_task, "tater_ws_reconnect", 4096, NULL, 4, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "websocket reconnect watchdog task create failed");
    }
    if (!s_timer_monitor_task) {
        BaseType_t timer_task_ok = xTaskCreate(timer_monitor_task, "tater_timer", 3072, NULL, 4, &s_timer_monitor_task);
        if (timer_task_ok != pdPASS) {
            s_timer_monitor_task = NULL;
            ESP_LOGE(TAG, "timer monitor task create failed");
        }
    }
}

bool tater_protocol_is_connected(void)
{
    return websocket_ready();
}

bool tater_protocol_voice_active(void)
{
    return s_voice_active;
}

bool tater_protocol_timer_is_ringing(void)
{
    return s_timer_ringing;
}

bool tater_protocol_timer_is_active(void)
{
    return s_timer_active;
}

void tater_protocol_timer_stop_from_device(void)
{
    if (!s_timer_ringing) {
        return;
    }
    ESP_LOGI(TAG, "timer stopped from device button");
    timer_clear_local(true, "stopped");
}

bool tater_protocol_can_start_local_wake(void)
{
    if (!websocket_ready() || s_voice_active || s_timer_ringing) {
        return false;
    }
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (settings && settings->muted) {
        return false;
    }
    if (tater_playback_is_playing()) {
        return settings && settings->barge_in_enabled;
    }
    return s_current_state != TATER_STATE_OTA && s_current_state != TATER_STATE_PROVISIONING;
}

const char *tater_protocol_device_id(void)
{
    return s_device_id;
}

const char *tater_protocol_device_name(void)
{
    return s_config.device_name[0] ? s_config.device_name : s_device_id;
}

const char *tater_protocol_room(void)
{
    return s_config.room;
}

const char *tater_protocol_server_url(void)
{
    return s_config.server_url;
}

const char *tater_protocol_token(void)
{
    return s_config.token;
}

void tater_protocol_send_status(const char *state)
{
    if (s_voice_active) {
        int64_t now_us = esp_timer_get_time();
        if (s_status_deferred_log_us == 0 || now_us - s_status_deferred_log_us > 10000000) {
            s_status_deferred_log_us = now_us;
            int64_t age_ms = s_voice_started_us > 0 ? (now_us - s_voice_started_us) / 1000 : 0;
            ESP_LOGI(
                TAG,
                "status deferred while voice active source=%s age_ms=%lld continued_reopen=%d queue=%u",
                s_voice_source[0] ? s_voice_source : "-",
                (long long)age_ms,
                s_voice_continued_reopen,
                (unsigned)audio_tx_queue_depth()
            );
        }
        return;
    }

    cJSON *root = new_envelope("status");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    const char *current_state = state_name(s_current_state);
    cJSON_AddStringToObject(payload, "state", current_state);
    if (state && strcmp(state, current_state) != 0) {
        cJSON_AddStringToObject(payload, "requested_state", state);
    }
    cJSON_AddNumberToObject(payload, "uptime_s", (int)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(payload, "free_heap", esp_get_free_heap_size());
    cJSON_AddBoolToObject(payload, "voice_active", s_voice_active);
    cJSON_AddBoolToObject(payload, "voice_start_pending", s_voice_start_pending);
    cJSON *timer = cJSON_CreateObject();
    cJSON_AddBoolToObject(timer, "active", s_timer_active);
    cJSON_AddBoolToObject(timer, "ringing", s_timer_ringing);
    cJSON_AddStringToObject(timer, "id", s_timer_id);
    cJSON_AddStringToObject(timer, "label", s_timer_label);
    if (s_timer_active && s_timer_deadline_us > 0) {
        int64_t remaining_ms = (s_timer_deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms < 0) {
            remaining_ms = 0;
        }
        cJSON_AddNumberToObject(timer, "remaining_ms", remaining_ms);
    }
    cJSON_AddItemToObject(payload, "timer", timer);
    cJSON_AddNumberToObject(payload, "audio_preroll_samples", s_audio_preroll_count);
    cJSON_AddBoolToObject(payload, "connected", websocket_ready());
    if (s_last_link_down_detail[0]) {
        cJSON_AddStringToObject(payload, "last_link_down", s_last_link_down_detail);
        cJSON_AddNumberToObject(payload, "last_link_down_age_ms", (esp_timer_get_time() - s_last_link_down_us) / 1000);
    }
    cJSON *transport = cJSON_CreateObject();
    cJSON_AddNumberToObject(transport, "audio_send_failures", s_audio_send_failures);
    cJSON_AddNumberToObject(transport, "audio_send_failure_total", s_audio_send_failure_total);
    cJSON_AddNumberToObject(transport, "last_audio_send_result", s_last_audio_send_result);
    cJSON_AddNumberToObject(transport, "last_audio_send_samples", s_last_audio_send_samples);
    cJSON_AddNumberToObject(transport, "audio_tx_queue_depth", audio_tx_queue_depth());
    cJSON_AddNumberToObject(transport, "audio_tx_queue_capacity", s_audio_tx_capacity);
    cJSON_AddNumberToObject(transport, "audio_tx_high_water", s_audio_tx_high_water);
    cJSON_AddNumberToObject(transport, "audio_tx_dropped", s_audio_tx_dropped);
    cJSON_AddNumberToObject(transport, "audio_tx_overruns", s_audio_tx_overruns);
    cJSON_AddNumberToObject(transport, "audio_tx_send_timeouts", s_audio_tx_send_timeouts);
    cJSON_AddNumberToObject(transport, "audio_tx_last_send_ms", s_audio_tx_last_send_ms);
    cJSON_AddNumberToObject(transport, "audio_tx_last_queue_depth", s_audio_tx_last_queue_depth);
    cJSON_AddNumberToObject(transport, "last_ws_error_type", s_last_ws_error_type);
    cJSON_AddNumberToObject(transport, "last_ws_tls_err", s_last_ws_tls_err);
    cJSON_AddNumberToObject(transport, "last_ws_stack_err", s_last_ws_stack_err);
    cJSON_AddNumberToObject(transport, "last_ws_sock_errno", s_last_ws_sock_errno);
    cJSON_AddNumberToObject(transport, "last_ws_http_status", s_last_ws_http_status);
    cJSON_AddItemToObject(payload, "transport", transport);
    cJSON_AddItemToObject(payload, "reset", reset_diag_json());
    tater_live_settings_add_status(payload);
    tater_wake_engine_add_status(payload);
    tater_audio_aec_stats_t aec = {0};
    if (tater_audio_aec_stats_snapshot(&aec)) {
        cJSON *aec_json = cJSON_CreateObject();
        cJSON_AddBoolToObject(aec_json, "enabled", aec.enabled);
        cJSON_AddBoolToObject(aec_json, "active", aec.active);
        cJSON_AddNumberToObject(aec_json, "processed_frames", aec.processed_frames);
        cJSON_AddNumberToObject(aec_json, "active_frames", aec.active_frames);
        cJSON_AddNumberToObject(aec_json, "reference_frames", aec.reference_frames);
        cJSON_AddNumberToObject(aec_json, "last_mic_level", aec.last_mic_level);
        cJSON_AddNumberToObject(aec_json, "last_reference_level", aec.last_reference_level);
        cJSON_AddNumberToObject(aec_json, "last_speaker_level", aec.last_speaker_level);
        cJSON_AddNumberToObject(aec_json, "last_output_gain", aec.last_output_gain);
        cJSON_AddNumberToObject(aec_json, "strength_percent", aec.strength_percent);
        cJSON_AddNumberToObject(aec_json, "delay_ms", aec.delay_ms);
        cJSON_AddItemToObject(payload, "aec", aec_json);
    }
    tater_audio_doa_t doa = {0};
    if (tater_audio_doa_snapshot(&doa)) {
        cJSON *xmos = cJSON_CreateObject();
        cJSON_AddBoolToObject(xmos, "valid", doa.valid);
        cJSON_AddNumberToObject(xmos, "confidence", doa.confidence);
        cJSON_AddNumberToObject(xmos, "sample_delay", doa.sample_delay);
        cJSON_AddNumberToObject(xmos, "vertical_delay", doa.vertical_delay);
        cJSON_AddNumberToObject(xmos, "angle_index", doa.angle_index);
        cJSON_AddBoolToObject(xmos, "four_mic", doa.four_mic);
        cJSON_AddNumberToObject(xmos, "energy", doa.energy);
        cJSON *mic_energy = cJSON_CreateArray();
        if (mic_energy) {
            for (size_t i = 0; i < 4; i++) {
                cJSON_AddItemToArray(mic_energy, cJSON_CreateNumber(doa.mic_energy[i]));
            }
            cJSON_AddItemToObject(xmos, "mic_energy", mic_energy);
        }
        cJSON_AddNumberToObject(xmos, "frame_counter", doa.frame_counter);
        cJSON_AddNumberToObject(xmos, "age_ms", doa.age_ms);
        cJSON_AddItemToObject(payload, "xmos_doa", xmos);
    }
    tater_audio_xmos_status_t xmos_status = {0};
    if (tater_audio_xmos_status_snapshot(&xmos_status)) {
        char installed[32] = {0};
        char target[32] = {0};
        format_xmos_version(
            target,
            sizeof(target),
            xmos_status.target_major,
            xmos_status.target_minor,
            xmos_status.target_patch,
            xmos_status.target_prerelease,
            xmos_status.target_counter
        );
        if (xmos_status.version_valid) {
            format_xmos_version(
                installed,
                sizeof(installed),
                xmos_status.major,
                xmos_status.minor,
                xmos_status.patch,
                xmos_status.prerelease,
                xmos_status.counter
            );
        }
        cJSON *xmos_fw = cJSON_CreateObject();
        cJSON_AddBoolToObject(xmos_fw, "version_valid", xmos_status.version_valid);
        cJSON_AddStringToObject(xmos_fw, "installed_version", xmos_status.version_valid ? installed : "");
        cJSON_AddStringToObject(xmos_fw, "target_version", target);
        cJSON_AddStringToObject(xmos_fw, "update_state", xmos_update_state_name(xmos_status.update_state));
        cJSON_AddBoolToObject(xmos_fw, "update_attempted", xmos_status.update_attempted);
        cJSON_AddBoolToObject(xmos_fw, "update_required", xmos_status.update_required);
        cJSON_AddNumberToObject(xmos_fw, "progress_percent", xmos_status.progress_percent);
        cJSON_AddNumberToObject(xmos_fw, "dfu_state", xmos_status.dfu_state);
        cJSON_AddNumberToObject(xmos_fw, "dfu_status", xmos_status.dfu_status);
        cJSON_AddItemToObject(payload, "xmos_firmware", xmos_fw);
    }

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(payload, "wifi_rssi", ap.rssi);
    }
    send_json(root);
}

void tater_protocol_start_voice_with_conversation(const char *wake_word, const char *source, const char *conversation_id)
{
    if (!websocket_ready()) {
        ESP_LOGW(TAG, "voice.start ignored: websocket disconnected");
        return;
    }
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (settings && settings->muted) {
        ESP_LOGW(TAG, "voice.start ignored: microphones muted");
        tater_protocol_send_log("warn", "Voice start ignored because microphones are muted.");
        tater_protocol_send_status("idle");
        return;
    }
    bool continued_reopen = source && strcmp(source, "continued_chat") == 0;
    uint32_t generation = 0;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    clear_audio_preroll_locked();
    audio_tx_clear_queue();
    s_voice_active = true;
    s_voice_start_pending = true;
    s_voice_continued_reopen = continued_reopen;
    s_voice_started_us = esp_timer_get_time();
    s_voice_generation++;
    generation = s_voice_generation;
    snprintf(s_voice_source, sizeof(s_voice_source), "%s", source ? source : "device");
    s_audio_send_logs = 0;
    s_audio_send_failures = 0;
    s_audio_send_failure_total = 0;
    s_audio_tx_high_water = 0;
    s_audio_tx_dropped = 0;
    s_audio_tx_overruns = 0;
    s_audio_tx_last_queue_depth = 0;
    s_audio_tx_last_send_ms = 0;
    s_audio_tx_send_timeouts = 0;
    s_last_audio_send_result = 0;
    s_last_audio_send_samples = 0;
    xSemaphoreGive(s_send_lock);
    emit_state(TATER_STATE_LISTENING, "local voice.start");

    cJSON *root = new_envelope("voice.start");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "wake_word", wake_word ? wake_word : "");
    cJSON_AddStringToObject(payload, "source", source ? source : "device");
    if (conversation_id && conversation_id[0]) {
        cJSON_AddStringToObject(payload, "conversation_id", conversation_id);
    }

    cJSON *format = cJSON_CreateObject();
    cJSON_AddNumberToObject(format, "rate", TATER_MIC_SAMPLE_RATE);
    cJSON_AddNumberToObject(format, "width", 2);
    cJSON_AddNumberToObject(format, "channels", 1);
    cJSON_AddItemToObject(payload, "audio_format", format);
    send_json(root);

    if (continued_reopen) {
        tater_voice_watchdog_args_t *request = (tater_voice_watchdog_args_t *)calloc(1, sizeof(tater_voice_watchdog_args_t));
        if (request) {
            request->generation = generation;
            if (xTaskCreate(continued_reopen_watchdog_task, "reopen_watchdog", 3072, request, 4, NULL) != pdPASS) {
                free(request);
                ESP_LOGW(TAG, "continued chat reopen watchdog task create failed");
            }
        } else {
            ESP_LOGW(TAG, "continued chat reopen watchdog alloc failed");
        }
    }
}

void tater_protocol_start_voice(const char *wake_word, const char *source)
{
    tater_protocol_start_voice_with_conversation(wake_word, source, "");
}

void tater_protocol_stop_voice(bool abort)
{
    if (!websocket_ready()) {
        clear_voice_capture_state();
        emit_state(TATER_STATE_IDLE, "local voice.stop offline");
        return;
    }
    if (!abort) {
        bool drained = audio_tx_wait_drained(pdMS_TO_TICKS(TATER_AUDIO_TX_DRAIN_WAIT_MS));
        if (!drained) {
            ESP_LOGW(TAG, "audio tx queue not fully drained before voice.stop depth=%u", (unsigned)audio_tx_queue_depth());
        }
    }
    cJSON *root = new_envelope("voice.stop");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddBoolToObject(payload, "abort", abort);
    send_json(root);
    clear_voice_capture_state();
    emit_state(TATER_STATE_IDLE, "local voice.stop");
}

void tater_protocol_send_audio(const int16_t *pcm, size_t sample_count)
{
    if (!websocket_ready() || !s_voice_active || !pcm || sample_count == 0) {
        return;
    }

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    if (s_voice_start_pending) {
        buffer_audio_preroll_locked(pcm, sample_count);
        xSemaphoreGive(s_send_lock);
        return;
    }
    xSemaphoreGive(s_send_lock);

    size_t depth = audio_tx_queue_depth();
    if (depth >= TATER_AUDIO_TX_CONGESTED_DEPTH) {
        audio_tx_count_dropped(1);
        if ((s_audio_tx_dropped == 1) || (s_audio_tx_dropped % 25 == 0)) {
            ESP_LOGW(
                TAG,
                "audio capture drop while tx congested depth=%u dropped=%u",
                (unsigned)depth,
                (unsigned)s_audio_tx_dropped
            );
        }
        return;
    }

    if (audio_tx_enqueue(pcm, sample_count, "capture")) {
        return;
    }

    int sent = -1;
    int64_t start_us = esp_timer_get_time();
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    if (websocket_ready() && s_voice_active && !s_voice_start_pending) {
        sent = send_audio_locked(pcm, sample_count, pdMS_TO_TICKS(TATER_AUDIO_TX_SEND_TIMEOUT_MS));
    }
    xSemaphoreGive(s_send_lock);
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    record_audio_send_result(sent, sample_count, elapsed_ms, 0);
}

void tater_protocol_send_log(const char *level, const char *message)
{
    cJSON *root = new_envelope("log");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "level", level ? level : "info");
    cJSON_AddStringToObject(payload, "message", message ? message : "");
    send_json(root);
}

static void continued_reopen_watchdog_task(void *arg)
{
    tater_voice_watchdog_args_t *request = (tater_voice_watchdog_args_t *)arg;
    uint32_t generation = request ? request->generation : 0;
    free(request);

    vTaskDelay(pdMS_TO_TICKS(TATER_CONTINUED_REOPEN_HARD_TIMEOUT_MS));

    bool timed_out = false;
    if (s_send_lock) {
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
        timed_out = s_voice_active && s_voice_continued_reopen && s_voice_generation == generation;
        xSemaphoreGive(s_send_lock);
    } else {
        timed_out = s_voice_active && s_voice_continued_reopen && s_voice_generation == generation;
    }

    if (timed_out) {
        ESP_LOGW(TAG, "continued chat reopen hard timeout; closing mic");
        tater_protocol_send_log("warn", "continued chat reopen timeout; closing mic");
        tater_protocol_stop_voice(true);
    }

    vTaskDelete(NULL);
}

static void continued_reopen_task(void *arg)
{
    tater_reopen_args_t *request = (tater_reopen_args_t *)arg;
    char conversation_id[sizeof(s_pending_reopen_conversation_id)] = {0};
    if (request) {
        snprintf(conversation_id, sizeof(conversation_id), "%s", request->conversation_id);
        free(request);
    }

    vTaskDelay(pdMS_TO_TICKS(350));
    if (conversation_id[0]) {
        if (!websocket_ready()) {
            ESP_LOGW(TAG, "continued chat reopen skipped: websocket not ready");
            emit_state(s_connected ? TATER_STATE_IDLE : TATER_STATE_DISCONNECTED, "continued reopen skipped");
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "continued chat reopening mic conversation_id=%s", conversation_id);
        tater_protocol_start_voice_with_conversation("", "continued_chat", conversation_id);
    }
    vTaskDelete(NULL);
}

void tater_protocol_send_playback_finished_status(bool ok, bool allow_reopen)
{
    bool should_reopen = ok && allow_reopen && s_pending_reopen && tater_live_settings_get()->continued_chat;
    bool return_armed = s_playback_return_armed;
    tater_state_t return_state = s_playback_return_state;
    char conversation_id[sizeof(s_pending_reopen_conversation_id)] = {0};
    if (should_reopen) {
        snprintf(conversation_id, sizeof(conversation_id), "%s", s_pending_reopen_conversation_id);
    }
    s_pending_reopen = false;
    s_pending_reopen_conversation_id[0] = '\0';
    s_playback_return_armed = false;
    s_playback_return_state = TATER_STATE_IDLE;
    clear_playback_visual_active();

    cJSON *root = new_envelope("playback.finished");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddBoolToObject(payload, "ok", ok);
    send_json(root);

    if (!should_reopen) {
        tater_state_t next_state = return_armed ? return_state : TATER_STATE_IDLE;
        if (!websocket_ready()) {
            next_state = TATER_STATE_DISCONNECTED;
        }
        const char *detail = return_armed ? "playback return" : (ok ? "playback finished" : "playback stopped");
        emit_state(next_state, detail);
    } else {
        emit_state(TATER_STATE_LISTENING, "continued reopen pending");
    }

    if (should_reopen && conversation_id[0]) {
        tater_reopen_args_t *request = (tater_reopen_args_t *)calloc(1, sizeof(tater_reopen_args_t));
        if (request) {
            snprintf(request->conversation_id, sizeof(request->conversation_id), "%s", conversation_id);
            if (xTaskCreate(continued_reopen_task, "tater_reopen", 4096, request, 5, NULL) != pdPASS) {
                free(request);
                ESP_LOGW(TAG, "continued chat reopen task create failed");
                emit_state(websocket_ready() ? TATER_STATE_IDLE : TATER_STATE_DISCONNECTED, "continued reopen task failed");
            }
        } else {
            ESP_LOGW(TAG, "continued chat reopen alloc failed");
            emit_state(websocket_ready() ? TATER_STATE_IDLE : TATER_STATE_DISCONNECTED, "continued reopen alloc failed");
        }
    }
}

void tater_protocol_send_playback_finished(void)
{
    tater_protocol_send_playback_finished_status(true, true);
}

void tater_protocol_send_ota_status(const char *status, int progress, const char *message)
{
    cJSON *root = new_envelope("ota.status");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "status", status ? status : "");
    cJSON_AddNumberToObject(payload, "progress", progress);
    cJSON_AddStringToObject(payload, "message", message ? message : "");
    send_json(root);
}
