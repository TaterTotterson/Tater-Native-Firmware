#include "tater_protocol.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "board.h"
#include "cJSON.h"
#include "esp_core_dump.h"
#include "esp_crt_bundle.h"
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
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "audio_aec.h"
#include "audio_i2s.h"
#include "native_settings.h"
#include "ota_update.h"
#include "playback.h"
#include "server_url.h"
#include "timer_sound_assets.h"
#include "wake_engine.h"

static const char *TAG = "tater_proto";

#if TATER_BOARD_SAT1
/*
 * The Espressif websocket transport doesn't retry a partial payload write
 * within the same frame, so larger frames can become malformed even though
 * the API reports the full message length. Keep enough receive space for
 * settings and command messages; outbound messages are explicitly fragmented.
 */
#define TATER_WS_BUFFER_SIZE 1024
#define TATER_WS_TX_FRAGMENT_SIZE 256
#define TATER_WAKE_VERIFY_TX_FRAGMENT_SIZE 512
#else
#define TATER_WS_BUFFER_SIZE 4096
#define TATER_WS_TX_FRAGMENT_SIZE 4096
#define TATER_WAKE_VERIFY_TX_FRAGMENT_SIZE 4096
#endif

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
static char s_hardware_id[13];
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
static int64_t s_link_down_started_us;
static int64_t s_last_reconnect_attempt_us;
static int64_t s_last_hello_us;
static bool s_hello_acked;
static bool s_playback_return_armed;
static tater_state_t s_playback_return_state = TATER_STATE_IDLE;
static bool s_playback_visual_active;
static int64_t s_playback_visual_started_us;
static bool s_tool_visual_hold;
#define TATER_MAX_LOCAL_TIMERS 8
#define TATER_TIMER_MAX_RING_MS (15 * 60 * 1000)

typedef struct {
    bool active;
    bool ringing;
    char id[48];
    char name[64];
    uint32_t original_duration_ms;
    int64_t deadline_us;
    int64_t ringing_started_us;
} tater_local_timer_t;

static tater_local_timer_t s_timers[TATER_MAX_LOCAL_TIMERS];
static SemaphoreHandle_t s_timer_lock;
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
static volatile bool s_ws_lifecycle_restart;
static uint32_t s_ws_restart_count;
static uint32_t s_ws_restart_failures;
static int s_last_json_send_result;
static uint32_t s_json_send_failure_total;
static uint32_t s_json_send_failure_streak;
static uint32_t s_json_send_failures_tolerated;
static char s_last_json_send_type[32];

#define TATER_MEDIA_TX_QUEUE_EVENTS 12

typedef enum {
    TATER_MEDIA_TX_STARTED = 0,
    TATER_MEDIA_TX_PLAYHEAD,
    TATER_MEDIA_TX_FINISHED,
} tater_media_tx_kind_t;

typedef struct {
    tater_media_tx_kind_t kind;
    char session_id[TATER_PLAYBACK_MEDIA_SESSION_ID_MAX];
    union {
        struct {
            char group_id[TATER_PLAYBACK_MEDIA_GROUP_ID_MAX];
            char channel[8];
            int64_t scheduled_start_us;
            int64_t actual_start_us;
        } started;
        struct {
            char group_id[TATER_PLAYBACK_MEDIA_GROUP_ID_MAX];
            char channel[8];
            uint64_t source_frames;
            uint64_t rendered_frames;
            uint64_t output_frames;
            uint32_t output_latency_frames;
            uint32_t buffered_frames;
            int64_t satellite_time_us;
            int64_t scheduled_start_us;
            int32_t correction_frames;
            bool rebuffering;
            uint32_t underrun_events;
            uint32_t rejoin_count;
            uint64_t rejoin_frames;
        } playhead;
        struct {
            bool ok;
        } finished;
    } payload;
} tater_media_tx_event_t;

static QueueHandle_t s_media_tx_queue;
static TaskHandle_t s_media_tx_task;
static uint32_t s_media_tx_high_water;
static uint32_t s_media_tx_dropped;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static int send_audio_locked(const int16_t *pcm, size_t sample_count, TickType_t timeout);
static void audio_tx_clear_queue(void);
static void audio_tx_task(void *arg);
static bool json_truthy(const cJSON *item);
static bool timer_any_ringing(void);
static void timer_monitor_task(void *arg);
static void continued_reopen_watchdog_task(void *arg);
static cJSON *new_envelope(const char *type);
static esp_err_t media_tx_init(void);
static void media_tx_start_task(void);

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
    bool task_with_caps;
} tater_reopen_args_t;

typedef struct {
    uint32_t generation;
    bool task_with_caps;
} tater_voice_watchdog_args_t;

#define TATER_WS_RECONNECT_AFTER_MS 30000
#define TATER_WS_RECONNECT_MIN_INTERVAL_MS 10000
#define TATER_WS_HELLO_ACK_TIMEOUT_MS 5000
#define TATER_WS_RESTART_FAILURE_LIMIT 3
#define TATER_JSON_SEND_LINK_DOWN_FAILURES 3
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
#define TATER_VOICE_START_ACK_GRACE_MS 500
#define TATER_CONTINUED_REOPEN_HARD_TIMEOUT_MS 12000

typedef struct {
    uint16_t samples;
    int16_t pcm[TATER_AUDIO_TX_BATCH_FRAMES];
} tater_audio_tx_chunk_t;

typedef struct {
    uint8_t *packet;
    size_t packet_size;
    uint32_t request_id;
    bool enforce;
    bool task_with_caps;
} tater_wake_verify_tx_args_t;

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

static BaseType_t create_transient_task(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *arg,
    UBaseType_t priority,
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
    BaseType_t created = xTaskCreateWithCaps(
        task,
        name,
        stack_depth,
        arg,
        priority,
        NULL,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (created == pdPASS) {
        return created;
    }
    if (task_with_caps) {
        *task_with_caps = false;
    }
#endif
    return xTaskCreate(task, name, stack_depth, arg, priority, NULL);
}

static void delete_transient_task(bool task_with_caps)
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
    if (timer_any_ringing() && state != TATER_STATE_TIMER && state != TATER_STATE_OTA && state != TATER_STATE_PROVISIONING) {
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
    int64_t now_us = esp_timer_get_time();
    s_last_link_down_us = now_us;
    if (s_link_down_started_us <= 0) {
        s_link_down_started_us = now_us;
    }
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
    return !s_ws_lifecycle_restart && s_client && s_connected && s_hello_acked;
}

static bool websocket_transport_ready(void)
{
    return !s_ws_lifecycle_restart && s_client && s_connected;
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
        .buffer_size = TATER_WS_BUFFER_SIZE,
        .ping_interval_sec = 30,
        .pingpong_timeout_sec = 30,
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms = 15000,
        .enable_close_reconnect = true,
        .headers = strlen(s_auth_header) > 0 ? s_auth_header : NULL,
    };
    if (strncasecmp(s_ws_url, "wss://", 6) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
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

static esp_err_t restart_websocket_client(bool recreate_client)
{
    s_ws_lifecycle_restart = true;
    s_ws_restart_count++;
    mark_link_down(recreate_client ? "websocket auth refresh" : "websocket lifecycle restart");

    /*
     * Stop new sends through websocket_ready(), then briefly acquire the send
     * lock to let an in-flight frame finish. Never hold this lock while
     * stopping the WebSocket task: its CONNECTED callback sends hello and may
     * need the same lock.
     */
    if (s_send_lock) {
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
        xSemaphoreGive(s_send_lock);
    }

    esp_websocket_client_handle_t client = s_client;
    if (client) {
        esp_err_t stop_err = esp_websocket_client_stop(client);
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "websocket lifecycle stop result=%s", esp_err_to_name(stop_err));
        }
    }

    if (recreate_client || !client) {
        if (client) {
            s_client = NULL;
            esp_err_t destroy_err = esp_websocket_client_destroy(client);
            if (destroy_err != ESP_OK) {
                ESP_LOGW(TAG, "websocket lifecycle destroy result=%s", esp_err_to_name(destroy_err));
            }
        }
        esp_err_t create_err = create_websocket_client();
        if (create_err != ESP_OK) {
            s_ws_lifecycle_restart = false;
            ESP_LOGE(TAG, "websocket lifecycle create result=%s", esp_err_to_name(create_err));
            return create_err;
        }
        s_recreate_client_on_reconnect = false;
    }

    vTaskDelay(pdMS_TO_TICKS(250));

    /*
     * Clear the gate before start so WEBSOCKET_EVENT_CONNECTED can send hello.
     * Other senders still see s_connected=false until that event arrives.
     */
    s_ws_lifecycle_restart = false;
    esp_err_t start_err = esp_websocket_client_start(s_client);
    if (start_err != ESP_OK) {
        ESP_LOGW(TAG, "websocket lifecycle start result=%s", esp_err_to_name(start_err));
    }
    return start_err;
}

static void reconnect_watchdog_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_ws_lifecycle_restart) {
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        int64_t since_attempt_us = s_last_reconnect_attempt_us > 0 ? now_us - s_last_reconnect_attempt_us : INT64_MAX;
        int64_t down_us = s_link_down_started_us > 0 ? now_us - s_link_down_started_us : now_us;
        int64_t hello_age_us = s_last_hello_us > 0 ? now_us - s_last_hello_us : INT64_MAX;
        bool recreate_client = s_recreate_client_on_reconnect || !s_client;
        const char *reason = "down";

        if (websocket_ready() && !recreate_client) {
            s_ws_restart_failures = 0;
            continue;
        }

        if (s_recreate_client_on_reconnect) {
            if (since_attempt_us < (int64_t)TATER_WS_RECONNECT_MIN_INTERVAL_MS * 1000) {
                continue;
            }
            reason = "auth_refresh";
        } else if (websocket_transport_ready() && !s_hello_acked) {
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
        ESP_LOGW(
            TAG,
            "websocket reconnect watchdog reason=%s down_ms=%lld client_connected=%d hello_acked=%d",
            reason,
            (long long)(down_us / 1000),
            s_connected,
            s_hello_acked
        );

        esp_err_t restart_err = restart_websocket_client(recreate_client);
        if (restart_err == ESP_OK) {
            s_ws_restart_failures = 0;
            continue;
        }

        s_ws_restart_failures++;
        ESP_LOGE(
            TAG,
            "websocket lifecycle recovery failed count=%lu/%u err=%s",
            (unsigned long)s_ws_restart_failures,
            TATER_WS_RESTART_FAILURE_LIMIT,
            esp_err_to_name(restart_err)
        );
        if (s_ws_restart_failures >= TATER_WS_RESTART_FAILURE_LIMIT) {
            ESP_LOGE(TAG, "websocket client cannot restart; rebooting for network stack recovery");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
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
    TickType_t next_send_tick = 0;

    while (true) {
        if (!audio_tx_pop(&chunk, NULL)) {
            next_send_tick = 0;
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

        /*
         * A preroll flush can put roughly half a second of PCM in the queue at
         * once. Sending that backlog in a tight loop fills the TCP window and
         * makes esp_websocket_client abort the whole connection on its next
         * write timeout. Pace queued PCM by its sample duration. Live capture
         * remains naturally paced because its next batch is not ready before
         * this deadline.
         */
        TickType_t now_tick = xTaskGetTickCount();
        if (next_send_tick != 0 && now_tick < next_send_tick) {
            vTaskDelay(next_send_tick - now_tick);
        }
        TickType_t send_tick = xTaskGetTickCount();

        int sent = -1;
        bool attempted = false;
        int64_t start_us = esp_timer_get_time();
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
        if (websocket_ready() && s_voice_active && !s_voice_start_pending) {
            attempted = true;
            sent = send_audio_locked(chunk.pcm, chunk.samples, pdMS_TO_TICKS(TATER_AUDIO_TX_SEND_TIMEOUT_MS));
        }
        xSemaphoreGive(s_send_lock);
        if (!attempted) {
            /* The server may end VAD while this chunk is waiting for pacing. */
            next_send_tick = 0;
            continue;
        }
        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
        record_audio_send_result(sent, chunk.samples, elapsed_ms, depth_after);
        if (sent >= (int)(chunk.samples * sizeof(int16_t))) {
            uint32_t audio_ms = ((uint32_t)chunk.samples * 1000U + (TATER_MIC_SAMPLE_RATE - 1U))
                / TATER_MIC_SAMPLE_RATE;
            TickType_t audio_ticks = pdMS_TO_TICKS(audio_ms);
            next_send_tick = send_tick + (audio_ticks > 0 ? audio_ticks : 1);
        } else {
            next_send_tick = 0;
        }

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

static int send_websocket_message_locked(
    const char *data,
    size_t len,
    bool binary,
    TickType_t timeout,
    size_t fragment_size)
{
    if (!data || len == 0 || len > INT_MAX || fragment_size == 0 || fragment_size > INT_MAX) {
        return -1;
    }
    if (len <= fragment_size) {
        return binary
            ? esp_websocket_client_send_bin(s_client, data, (int)len, timeout)
            : esp_websocket_client_send_text(s_client, data, (int)len, timeout);
    }

    size_t offset = 0;
    size_t chunk_len = fragment_size;
    int sent = binary
        ? esp_websocket_client_send_bin_partial(s_client, data, (int)chunk_len, timeout)
        : esp_websocket_client_send_text_partial(s_client, data, (int)chunk_len, timeout);
    if (sent != (int)chunk_len) {
        return -1;
    }
    offset += chunk_len;

    while (offset < len) {
        chunk_len = len - offset;
        if (chunk_len > fragment_size) {
            chunk_len = fragment_size;
        }
        sent = esp_websocket_client_send_cont_msg(
            s_client,
            data + offset,
            (int)chunk_len,
            timeout
        );
        if (sent != (int)chunk_len) {
            return -1;
        }
        offset += chunk_len;
    }

    if (esp_websocket_client_send_fin(s_client, timeout) < 0) {
        return -1;
    }
    return (int)len;
}

static int send_audio_locked(const int16_t *pcm, size_t sample_count, TickType_t timeout)
{
    return send_websocket_message_locked(
        (const char *)pcm,
        sample_count * sizeof(int16_t),
        true,
        timeout,
        TATER_WS_TX_FRAGMENT_SIZE
    );
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8) & 0xffU);
    dst[2] = (uint8_t)((value >> 16) & 0xffU);
    dst[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void wake_verify_tx_task(void *arg)
{
    tater_wake_verify_tx_args_t *request = (tater_wake_verify_tx_args_t *)arg;
    uint8_t *packet = request ? request->packet : NULL;
    size_t packet_size = request ? request->packet_size : 0;
    uint32_t request_id = request ? request->request_id : 0;
    bool enforce = request && request->enforce;
    bool task_with_caps = request && request->task_with_caps;
    int sent = -1;
    int64_t started_us = esp_timer_get_time();

    if (request && packet && packet_size > 0) {
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
        if (websocket_ready()) {
            sent = send_websocket_message_locked(
                (const char *)packet,
                packet_size,
                true,
                pdMS_TO_TICKS(250),
                TATER_WAKE_VERIFY_TX_FRAGMENT_SIZE
            );
        }
        xSemaphoreGive(s_send_lock);
    }
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
    ESP_LOGI(
        TAG,
        "wake verifier upload request=%lu bytes=%u result=%d elapsed_ms=%u enforce=%d",
        (unsigned long)request_id,
        (unsigned)packet_size,
        sent,
        (unsigned)elapsed_ms,
        enforce
    );
    if (packet) {
        heap_caps_free(packet);
    }
    free(request);
    if (sent != (int)packet_size && enforce) {
        tater_wake_engine_verification_result(request_id, true, true, "upload_fail_open");
    }
    delete_transient_task(task_with_caps);
}

uint32_t tater_protocol_send_wake_verification(
    uint32_t request_id,
    const int16_t *pcm,
    size_t sample_count,
    bool enforce
)
{
    const size_t header_size = 20;
    if (
        request_id == 0
        || !websocket_ready()
        || !pcm
        || sample_count == 0
        || sample_count > (size_t)(TATER_MIC_SAMPLE_RATE * 2)
    ) {
        return 0;
    }
    size_t pcm_size = sample_count * sizeof(int16_t);
    size_t packet_size = header_size + pcm_size;
    uint8_t *packet = (uint8_t *)heap_caps_malloc(packet_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!packet) {
        packet = (uint8_t *)heap_caps_malloc(packet_size, MALLOC_CAP_8BIT);
    }
    if (!packet) {
        ESP_LOGW(TAG, "wake verifier packet alloc failed bytes=%u", (unsigned)packet_size);
        return 0;
    }

    memcpy(packet, "TWV1", 4);
    packet[4] = 1;
    packet[5] = 1;
    packet[6] = enforce ? 1 : 0;
    packet[7] = 0;
    write_le32(packet + 8, request_id);
    write_le32(packet + 12, TATER_MIC_SAMPLE_RATE);
    write_le32(packet + 16, (uint32_t)sample_count);
    memcpy(packet + header_size, pcm, pcm_size);

    tater_wake_verify_tx_args_t *request = (tater_wake_verify_tx_args_t *)calloc(1, sizeof(tater_wake_verify_tx_args_t));
    if (!request) {
        heap_caps_free(packet);
        return 0;
    }
    request->packet = packet;
    request->packet_size = packet_size;
    request->request_id = request_id;
    request->enforce = enforce;
    if (create_transient_task(
            wake_verify_tx_task,
            "wake_verify_tx",
            4096,
            request,
            5,
            &request->task_with_caps
        ) != pdPASS) {
        heap_caps_free(packet);
        free(request);
        ESP_LOGW(TAG, "wake verifier upload task create failed");
        return 0;
    }
    return request_id;
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

static void build_device_identity(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(
        s_hardware_id,
        sizeof(s_hardware_id),
        "%02x%02x%02x%02x%02x%02x",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );
    snprintf(
        s_device_id,
        sizeof(s_device_id),
        "%s-%02x%02x%02x",
        TATER_DEVICE_ID_PREFIX,
        mac[3],
        mac[4],
        mac[5]
    );
}

static bool build_ws_url(void)
{
    if (!tater_server_build_ws_url(s_config.server_url, s_ws_url, sizeof(s_ws_url))) {
        s_ws_url[0] = '\0';
        ESP_LOGE(TAG, "invalid Tater server URL: %s", s_config.server_url);
        return false;
    }
    return true;
}

static bool should_log_send(const char *type, int sent)
{
    if (sent < 0) {
        return false;
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

    bool attempted = false;
    bool send_failed = false;
    bool transport_connected_after = false;
    bool mark_down = false;
    uint32_t failure_streak = 0;
    int sent = -1;
    size_t wire_len = strlen(wire);
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    if (is_hello ? websocket_transport_ready() : websocket_ready()) {
        attempted = true;
        sent = send_websocket_message_locked(
            wire,
            wire_len,
            false,
            pdMS_TO_TICKS(1000),
            TATER_WS_TX_FRAGMENT_SIZE
        );
        s_last_json_send_result = sent;
        strlcpy(s_last_json_send_type, type, sizeof(s_last_json_send_type));
        send_failed = sent != (int)wire_len;
        if (!send_failed) {
            s_json_send_failure_streak = 0;
        } else {
            s_json_send_failure_total++;
            s_json_send_failure_streak++;
            failure_streak = s_json_send_failure_streak;
            transport_connected_after =
                s_client && esp_websocket_client_is_connected(s_client);
            mark_down =
                !transport_connected_after
                || failure_streak >= TATER_JSON_SEND_LINK_DOWN_FAILURES;
            if (!mark_down) {
                s_json_send_failures_tolerated++;
            }
        }
    }
    xSemaphoreGive(s_send_lock);
    if (attempted && !send_failed && should_log_send(type, sent)) {
        ESP_LOGI(TAG, "json send type=%s bytes=%u result=%d", type, (unsigned)wire_len, sent);
    }
    if (attempted && send_failed) {
        ESP_LOGW(
            TAG,
            "json send failed type=%s bytes=%u result=%d streak=%lu/%u transport_connected=%d",
            type,
            (unsigned)wire_len,
            sent,
            (unsigned long)failure_streak,
            TATER_JSON_SEND_LINK_DOWN_FAILURES,
            transport_connected_after
        );
        if (mark_down) {
            mark_link_down("websocket send failure threshold");
        }
    }
    cJSON_free(wire);
    return sent;
}

static int send_media_tx_event_now(const tater_media_tx_event_t *event)
{
    if (!event) {
        return -1;
    }

    cJSON *root = NULL;
    cJSON *payload = NULL;
    switch (event->kind) {
    case TATER_MEDIA_TX_STARTED:
        root = new_envelope("media.session.started");
        payload = cJSON_GetObjectItem(root, "payload");
        cJSON_AddStringToObject(payload, "session_id", event->session_id);
        cJSON_AddStringToObject(payload, "group_id", event->payload.started.group_id);
        cJSON_AddStringToObject(payload, "channel", event->payload.started.channel);
        cJSON_AddNumberToObject(payload, "sample_rate_hz", TATER_SPK_SAMPLE_RATE);
        cJSON_AddNumberToObject(
            payload,
            "scheduled_start_us",
            (double)event->payload.started.scheduled_start_us
        );
        cJSON_AddNumberToObject(
            payload,
            "actual_start_us",
            (double)event->payload.started.actual_start_us
        );
        cJSON_AddNumberToObject(
            payload,
            "late_by_us",
            (double)(
                event->payload.started.actual_start_us
                - event->payload.started.scheduled_start_us
            )
        );
        break;
    case TATER_MEDIA_TX_PLAYHEAD:
        root = new_envelope("media.session.playhead");
        payload = cJSON_GetObjectItem(root, "payload");
        cJSON_AddStringToObject(payload, "session_id", event->session_id);
        cJSON_AddStringToObject(payload, "group_id", event->payload.playhead.group_id);
        cJSON_AddStringToObject(payload, "channel", event->payload.playhead.channel);
        cJSON_AddNumberToObject(payload, "sample_rate_hz", TATER_SPK_SAMPLE_RATE);
        cJSON_AddNumberToObject(payload, "source_frames", (double)event->payload.playhead.source_frames);
        cJSON_AddNumberToObject(payload, "rendered_frames", (double)event->payload.playhead.rendered_frames);
        cJSON_AddNumberToObject(payload, "output_frames", (double)event->payload.playhead.output_frames);
        cJSON_AddNumberToObject(payload, "output_latency_frames", event->payload.playhead.output_latency_frames);
        cJSON_AddNumberToObject(payload, "buffered_frames", event->payload.playhead.buffered_frames);
        cJSON_AddNumberToObject(payload, "satellite_time_us", (double)event->payload.playhead.satellite_time_us);
        cJSON_AddNumberToObject(payload, "scheduled_start_us", (double)event->payload.playhead.scheduled_start_us);
        cJSON_AddNumberToObject(payload, "correction_frames", event->payload.playhead.correction_frames);
        cJSON_AddBoolToObject(payload, "rebuffering", event->payload.playhead.rebuffering);
        cJSON_AddNumberToObject(payload, "underrun_events", event->payload.playhead.underrun_events);
        cJSON_AddNumberToObject(payload, "rejoin_count", event->payload.playhead.rejoin_count);
        cJSON_AddNumberToObject(payload, "rejoin_frames", (double)event->payload.playhead.rejoin_frames);
        break;
    case TATER_MEDIA_TX_FINISHED:
        root = new_envelope("media.session.finished");
        payload = cJSON_GetObjectItem(root, "payload");
        cJSON_AddStringToObject(payload, "session_id", event->session_id);
        cJSON_AddBoolToObject(payload, "ok", event->payload.finished.ok);
        break;
    default:
        return -1;
    }
    return send_json(root);
}

static void media_tx_worker(void *arg)
{
    (void)arg;
    tater_media_tx_event_t event;
    for (;;) {
        if (
            s_media_tx_queue
            && xQueueReceive(s_media_tx_queue, &event, portMAX_DELAY) == pdTRUE
        ) {
            (void)send_media_tx_event_now(&event);
        }
    }
}

static esp_err_t media_tx_init(void)
{
    if (s_media_tx_queue) {
        return ESP_OK;
    }
    s_media_tx_queue = xQueueCreate(
        TATER_MEDIA_TX_QUEUE_EVENTS,
        sizeof(tater_media_tx_event_t)
    );
    return s_media_tx_queue ? ESP_OK : ESP_ERR_NO_MEM;
}

static void media_tx_start_task(void)
{
    if (!s_media_tx_queue || s_media_tx_task) {
        return;
    }
    BaseType_t ok = xTaskCreate(
        media_tx_worker,
        "tater_media_tx",
        6144,
        NULL,
        4,
        &s_media_tx_task
    );
    if (ok != pdPASS) {
        s_media_tx_task = NULL;
        ESP_LOGE(TAG, "media telemetry task create failed");
    }
}

static bool media_tx_enqueue(const tater_media_tx_event_t *event)
{
    if (!event) {
        return false;
    }
    if (!s_media_tx_queue || !s_media_tx_task) {
        return send_media_tx_event_now(event) >= 0;
    }
    if (xQueueSend(s_media_tx_queue, event, 0) == pdTRUE) {
        UBaseType_t depth = uxQueueMessagesWaiting(s_media_tx_queue);
        if (depth > s_media_tx_high_water) {
            s_media_tx_high_water = depth;
        }
        return true;
    }

    s_media_tx_dropped++;
    if (event->kind == TATER_MEDIA_TX_PLAYHEAD) {
        return false;
    }

    /*
     * A stalled connection can fill the queue with expendable playheads.
     * Preserve lifecycle progress without ever blocking the audio task.
     */
    tater_media_tx_event_t dropped;
    (void)xQueueReceive(s_media_tx_queue, &dropped, 0);
    return xQueueSend(s_media_tx_queue, event, 0) == pdTRUE;
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

static void timer_lock(void)
{
    if (s_timer_lock) {
        xSemaphoreTake(s_timer_lock, portMAX_DELAY);
    }
}

static void timer_unlock(void)
{
    if (s_timer_lock) {
        xSemaphoreGive(s_timer_lock);
    }
}

static int timer_active_count_locked(void)
{
    int count = 0;
    for (size_t i = 0; i < TATER_MAX_LOCAL_TIMERS; i++) {
        if (s_timers[i].active) {
            count++;
        }
    }
    return count;
}

static int timer_ringing_count_locked(void)
{
    int count = 0;
    for (size_t i = 0; i < TATER_MAX_LOCAL_TIMERS; i++) {
        if (s_timers[i].active && s_timers[i].ringing) {
            count++;
        }
    }
    return count;
}

static bool timer_any_ringing(void)
{
    timer_lock();
    bool ringing = timer_ringing_count_locked() > 0;
    timer_unlock();
    return ringing;
}

static bool timer_any_active(void)
{
    timer_lock();
    bool active = timer_active_count_locked() > 0;
    timer_unlock();
    return active;
}

static int64_t timer_remaining_ms(const tater_local_timer_t *timer)
{
    if (!timer || !timer->active || timer->ringing || timer->deadline_us <= 0) {
        return 0;
    }
    int64_t remaining_ms = (timer->deadline_us - esp_timer_get_time()) / 1000;
    return remaining_ms > 0 ? remaining_ms : 0;
}

static cJSON *timer_json(const tater_local_timer_t *timer)
{
    cJSON *row = cJSON_CreateObject();
    if (!row || !timer) {
        return row;
    }
    cJSON_AddStringToObject(row, "id", timer->id);
    cJSON_AddStringToObject(row, "name", timer->name);
    cJSON_AddStringToObject(row, "label", timer->name);
    cJSON_AddStringToObject(row, "state", timer->ringing ? "ringing" : "armed");
    cJSON_AddBoolToObject(row, "active", timer->active);
    cJSON_AddBoolToObject(row, "ringing", timer->ringing);
    cJSON_AddNumberToObject(row, "original_duration_ms", timer->original_duration_ms);
    cJSON_AddNumberToObject(row, "duration_ms", timer->original_duration_ms);
    cJSON_AddNumberToObject(row, "remaining_ms", (double)timer_remaining_ms(timer));
    return row;
}

static void timer_emit_event(const char *event, const tater_local_timer_t *timer)
{
    if (!timer) {
        return;
    }
    cJSON *root = new_envelope("timer.event");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "event", event ? event : "");
    cJSON_AddStringToObject(payload, "id", timer->id);
    cJSON_AddStringToObject(payload, "name", timer->name);
    cJSON_AddStringToObject(payload, "label", timer->name);
    cJSON_AddStringToObject(payload, "state", timer->ringing ? "ringing" : (timer->active ? "armed" : "stopped"));
    cJSON_AddBoolToObject(payload, "active", timer->active);
    cJSON_AddBoolToObject(payload, "ringing", timer->ringing);
    cJSON_AddNumberToObject(payload, "original_duration_ms", timer->original_duration_ms);
    cJSON_AddNumberToObject(payload, "remaining_ms", (double)timer_remaining_ms(timer));
    send_json(root);
}

static cJSON *timer_result_payload(
    const char *reply_to,
    const char *action,
    bool ok,
    const char *code,
    const char *message
)
{
    cJSON *root = new_envelope("timer.result");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "reply_to", reply_to ? reply_to : "");
    cJSON_AddStringToObject(payload, "action", action ? action : "");
    cJSON_AddBoolToObject(payload, "ok", ok);
    if (code && code[0]) {
        cJSON_AddStringToObject(payload, "code", code);
    }
    if (message && message[0]) {
        cJSON_AddStringToObject(payload, "message", message);
    }
    return root;
}

static void timer_send_list_result(const char *reply_to)
{
    cJSON *root = timer_result_payload(reply_to, "list", true, "", "");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON *timers = cJSON_CreateArray();
    int count = 0;
    timer_lock();
    for (size_t i = 0; i < TATER_MAX_LOCAL_TIMERS; i++) {
        if (!s_timers[i].active) {
            continue;
        }
        cJSON_AddItemToArray(timers, timer_json(&s_timers[i]));
        count++;
    }
    timer_unlock();
    cJSON_AddItemToObject(payload, "timers", timers);
    cJSON_AddNumberToObject(payload, "count", count);
    send_json(root);
}

static bool timer_payload_matches(const cJSON *payload, const tater_local_timer_t *timer, bool *has_criteria)
{
    if (!payload || !timer || !timer->active) {
        return false;
    }
    bool criteria = false;
    bool matches = true;
    const cJSON *ids = cJSON_GetObjectItem(payload, "ids");
    if (cJSON_IsArray(ids) && cJSON_GetArraySize(ids) > 0) {
        criteria = true;
        bool id_match = false;
        const cJSON *candidate = NULL;
        cJSON_ArrayForEach(candidate, ids) {
            if (cJSON_IsString(candidate) && candidate->valuestring && strcmp(candidate->valuestring, timer->id) == 0) {
                id_match = true;
                break;
            }
        }
        matches = matches && id_match;
    }

    const cJSON *id_item = cJSON_GetObjectItem(payload, "id");
    if (cJSON_IsString(id_item) && id_item->valuestring && id_item->valuestring[0]) {
        criteria = true;
        matches = matches && strcmp(id_item->valuestring, timer->id) == 0;
    }
    const cJSON *name_item = cJSON_GetObjectItem(payload, "name");
    if (!cJSON_IsString(name_item) || !name_item->valuestring) {
        name_item = cJSON_GetObjectItem(payload, "label");
    }
    if (cJSON_IsString(name_item) && name_item->valuestring && name_item->valuestring[0]) {
        criteria = true;
        matches = matches && strcasecmp(name_item->valuestring, timer->name) == 0;
    }
    uint32_t duration_ms = timer_payload_duration_ms(
        payload,
        "original_duration_ms",
        "original_duration_s",
        0
    );
    if (duration_ms > 0) {
        criteria = true;
        matches = matches && duration_ms == timer->original_duration_ms;
    }
    if (has_criteria) {
        *has_criteria = criteria;
    }
    return matches;
}

static size_t timer_select_locked(const cJSON *payload, size_t *indices, size_t capacity, bool *ambiguous)
{
    bool select_all = json_truthy(cJSON_GetObjectItem(payload, "all"));
    const cJSON *ids = cJSON_GetObjectItem(payload, "ids");
    bool explicit_id_list = cJSON_IsArray(ids) && cJSON_GetArraySize(ids) > 0;
    bool has_criteria = false;
    for (size_t i = 0; i < TATER_MAX_LOCAL_TIMERS; i++) {
        bool row_has_criteria = false;
        (void)timer_payload_matches(payload, &s_timers[i], &row_has_criteria);
        has_criteria = has_criteria || row_has_criteria;
    }

    size_t count = 0;
    if (select_all || has_criteria) {
        for (size_t i = 0; i < TATER_MAX_LOCAL_TIMERS && count < capacity; i++) {
            if (!s_timers[i].active) {
                continue;
            }
            if (select_all || timer_payload_matches(payload, &s_timers[i], NULL)) {
                indices[count++] = i;
            }
        }
        if (has_criteria && !select_all && !explicit_id_list && count > 1) {
            if (ambiguous) {
                *ambiguous = true;
            }
            return 0;
        }
        return count;
    }

    int ringing_count = timer_ringing_count_locked();
    if (ringing_count > 0) {
        for (size_t i = 0; i < TATER_MAX_LOCAL_TIMERS && count < capacity; i++) {
            if (s_timers[i].active && s_timers[i].ringing) {
                indices[count++] = i;
            }
        }
        return count;
    }

    if (timer_active_count_locked() == 1) {
        for (size_t i = 0; i < TATER_MAX_LOCAL_TIMERS; i++) {
            if (s_timers[i].active) {
                indices[count++] = i;
                break;
            }
        }
        return count;
    }
    if (ambiguous) {
        *ambiguous = timer_active_count_locked() > 1;
    }
    return 0;
}

static void timer_alarm_task(void *arg)
{
    (void)arg;
    const tater_timer_sound_asset_t *alarm = tater_timer_sound_asset();
    while (timer_any_ringing()) {
        emit_state(TATER_STATE_TIMER, "timer ringing");
        if (!tater_playback_is_playing()) {
            esp_err_t err = alarm
                ? tater_playback_play_wav_data_local(
                    alarm->data,
                    (size_t)(alarm->end - alarm->data),
                    "zen_timer_alarm"
                )
                : tater_playback_play_tone_local(880, 420, 80);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "timer alarm playback failed: %s", esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
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

static void timer_begin_ringing(void)
{
    if (s_voice_active) {
        tater_protocol_stop_voice(true);
    }
    if (tater_playback_is_playing()) {
        tater_playback_stop();
    }
    tater_wake_engine_set_timer_stop_mode(true);
    emit_state(TATER_STATE_TIMER, "timer ringing");
    timer_start_alarm_task();
}

static void timer_finish_ringing_if_idle(bool was_ringing)
{
    if (!was_ringing || timer_any_ringing()) {
        return;
    }
    tater_wake_engine_set_timer_stop_mode(false);
    if (tater_playback_is_playing()) {
        tater_playback_stop();
    }
    if (s_current_state == TATER_STATE_TIMER) {
        emit_state(websocket_ready() ? TATER_STATE_IDLE : TATER_STATE_DISCONNECTED, "timer stopped");
    }
}

static void timer_start_from_payload(const cJSON *payload, const char *reply_to, bool replace_existing)
{
    uint32_t original_duration_ms = timer_payload_duration_ms(
        payload,
        "original_duration_ms",
        "original_duration_s",
        0
    );
    if (original_duration_ms == 0) {
        original_duration_ms = timer_payload_duration_ms(payload, "duration_ms", "duration_s", 0);
    }
    uint32_t countdown_ms = timer_payload_duration_ms(payload, "remaining_ms", "remaining_s", 0);
    if (countdown_ms == 0) {
        countdown_ms = timer_payload_duration_ms(
            payload,
            "duration_ms",
            "duration_s",
            original_duration_ms
        );
    }
    if (countdown_ms == 0) {
        cJSON *root = timer_result_payload(reply_to, "start", false, "invalid_duration", "Timer duration must be greater than zero.");
        send_json(root);
        return;
    }
    if (original_duration_ms == 0) {
        original_duration_ms = countdown_ms;
    }

    char requested_id[48] = {0};
    char requested_name[64] = {0};
    const cJSON *id_item = cJSON_GetObjectItem(payload, "id");
    const cJSON *name_item = cJSON_GetObjectItem(payload, "name");
    if (!cJSON_IsString(name_item) || !name_item->valuestring) {
        name_item = cJSON_GetObjectItem(payload, "label");
    }
    if (cJSON_IsString(id_item) && id_item->valuestring) {
        strlcpy(requested_id, id_item->valuestring, sizeof(requested_id));
    }
    if (!requested_id[0]) {
        make_id(requested_id, sizeof(requested_id));
    }
    if (cJSON_IsString(name_item) && name_item->valuestring) {
        strlcpy(requested_name, name_item->valuestring, sizeof(requested_name));
    }

    tater_local_timer_t snapshot = {0};
    bool duplicate = false;
    bool full = false;
    bool was_ringing = false;
    timer_lock();
    int target = -1;
    for (size_t i = 0; i < TATER_MAX_LOCAL_TIMERS; i++) {
        if (s_timers[i].active && strcmp(s_timers[i].id, requested_id) == 0) {
            target = (int)i;
            duplicate = !replace_existing;
            break;
        }
        if (target < 0 && !s_timers[i].active) {
            target = (int)i;
        }
    }
    if (target < 0) {
        full = true;
    } else if (duplicate) {
        snapshot = s_timers[target];
    } else {
        tater_local_timer_t *timer = &s_timers[target];
        was_ringing = timer->active && timer->ringing;
        char existing_name[sizeof(timer->name)] = {0};
        strlcpy(existing_name, timer->name, sizeof(existing_name));
        memset(timer, 0, sizeof(*timer));
        timer->active = true;
        strlcpy(timer->id, requested_id, sizeof(timer->id));
        strlcpy(
            timer->name,
            requested_name[0] ? requested_name : existing_name,
            sizeof(timer->name)
        );
        timer->original_duration_ms = original_duration_ms;
        timer->deadline_us = esp_timer_get_time() + ((int64_t)countdown_ms * 1000LL);
        snapshot = *timer;
    }
    timer_unlock();

    if (full) {
        cJSON *root = timer_result_payload(reply_to, "start", false, "timer_limit", "This satellite already has the maximum number of timers.");
        send_json(root);
        return;
    }

    cJSON *root = timer_result_payload(reply_to, "start", true, duplicate ? "already_exists" : "", "");
    cJSON *result = cJSON_GetObjectItem(root, "payload");
    cJSON_AddItemToObject(result, "timer", timer_json(&snapshot));
    send_json(root);
    if (!duplicate) {
        timer_emit_event(replace_existing ? "updated" : "armed", &snapshot);
        ESP_LOGI(
            TAG,
            "timer %s id=%s name=%s countdown_ms=%lu original_duration_ms=%lu",
            replace_existing ? "updated" : "armed",
            snapshot.id,
            snapshot.name,
            (unsigned long)countdown_ms,
            (unsigned long)original_duration_ms
        );
    }
    timer_finish_ringing_if_idle(was_ringing);
}

static void timer_cancel_from_payload(const cJSON *payload, const char *reply_to, const char *event)
{
    size_t indices[TATER_MAX_LOCAL_TIMERS] = {0};
    tater_local_timer_t affected[TATER_MAX_LOCAL_TIMERS] = {0};
    bool ambiguous = false;
    bool was_ringing = false;
    size_t count = 0;

    timer_lock();
    count = timer_select_locked(payload, indices, TATER_MAX_LOCAL_TIMERS, &ambiguous);
    for (size_t i = 0; i < count; i++) {
        size_t index = indices[i];
        affected[i] = s_timers[index];
        was_ringing = was_ringing || s_timers[index].ringing;
        memset(&s_timers[index], 0, sizeof(s_timers[index]));
    }
    timer_unlock();

    const char *code = ambiguous ? "ambiguous" : (count == 0 ? "not_found" : "");
    const char *message = ambiguous
        ? "More than one timer is running; specify a timer name or duration."
        : (count == 0 ? "No matching timer is running." : "");
    cJSON *root = timer_result_payload(reply_to, "cancel", true, code, message);
    cJSON *result = cJSON_GetObjectItem(root, "payload");
    cJSON *timers = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(timers, timer_json(&affected[i]));
    }
    cJSON_AddItemToObject(result, "timers", timers);
    cJSON_AddNumberToObject(result, "affected", count);
    send_json(root);

    for (size_t i = 0; i < count; i++) {
        affected[i].active = false;
        affected[i].ringing = false;
        affected[i].deadline_us = 0;
        timer_emit_event(event ? event : "cancelled", &affected[i]);
    }
    timer_finish_ringing_if_idle(was_ringing);
}

static void timer_snooze_from_payload(const cJSON *payload, const char *reply_to)
{
    uint32_t duration_ms = timer_payload_duration_ms(payload, "duration_ms", "duration_s", 5 * 60 * 1000);
    size_t indices[TATER_MAX_LOCAL_TIMERS] = {0};
    tater_local_timer_t affected[TATER_MAX_LOCAL_TIMERS] = {0};
    bool ambiguous = false;
    bool was_ringing = false;
    size_t count = 0;

    timer_lock();
    count = timer_select_locked(payload, indices, TATER_MAX_LOCAL_TIMERS, &ambiguous);
    for (size_t i = 0; i < count; i++) {
        tater_local_timer_t *timer = &s_timers[indices[i]];
        was_ringing = was_ringing || timer->ringing;
        timer->ringing = false;
        timer->ringing_started_us = 0;
        timer->original_duration_ms = duration_ms;
        timer->deadline_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000LL);
        affected[i] = *timer;
    }
    timer_unlock();

    const char *code = ambiguous ? "ambiguous" : (count == 0 ? "not_found" : "");
    const char *message = ambiguous
        ? "More than one timer is running; specify a timer name or duration."
        : (count == 0 ? "No matching timer is running." : "");
    cJSON *root = timer_result_payload(reply_to, "snooze", true, code, message);
    cJSON *result = cJSON_GetObjectItem(root, "payload");
    cJSON *timers = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(timers, timer_json(&affected[i]));
    }
    cJSON_AddItemToObject(result, "timers", timers);
    cJSON_AddNumberToObject(result, "affected", count);
    send_json(root);
    for (size_t i = 0; i < count; i++) {
        timer_emit_event("snoozed", &affected[i]);
    }
    timer_finish_ringing_if_idle(was_ringing);
}

static void timer_force_alarm_from_payload(const cJSON *payload)
{
    size_t indices[TATER_MAX_LOCAL_TIMERS] = {0};
    size_t count = 0;
    tater_local_timer_t expired[TATER_MAX_LOCAL_TIMERS] = {0};
    timer_lock();
    count = timer_select_locked(payload, indices, TATER_MAX_LOCAL_TIMERS, NULL);
    for (size_t i = 0; i < count; i++) {
        tater_local_timer_t *timer = &s_timers[indices[i]];
        timer->ringing = true;
        timer->deadline_us = 0;
        timer->ringing_started_us = esp_timer_get_time();
        expired[i] = *timer;
    }
    timer_unlock();
    if (count == 0) {
        return;
    }
    timer_begin_ringing();
    for (size_t i = 0; i < count; i++) {
        timer_emit_event("expired", &expired[i]);
    }
}

static void timer_monitor_task(void *arg)
{
    (void)arg;
    while (true) {
        tater_local_timer_t expired[TATER_MAX_LOCAL_TIMERS] = {0};
        tater_local_timer_t auto_stopped[TATER_MAX_LOCAL_TIMERS] = {0};
        size_t expired_count = 0;
        size_t auto_stopped_count = 0;
        int64_t now_us = esp_timer_get_time();
        timer_lock();
        for (size_t i = 0; i < TATER_MAX_LOCAL_TIMERS; i++) {
            tater_local_timer_t *timer = &s_timers[i];
            if (!timer->active) {
                continue;
            }
            if (!timer->ringing && timer->deadline_us > 0 && now_us >= timer->deadline_us) {
                timer->ringing = true;
                timer->deadline_us = 0;
                timer->ringing_started_us = now_us;
                expired[expired_count++] = *timer;
            } else if (timer->ringing
                    && timer->ringing_started_us > 0
                    && ((now_us - timer->ringing_started_us) / 1000) >= TATER_TIMER_MAX_RING_MS) {
                auto_stopped[auto_stopped_count++] = *timer;
                memset(timer, 0, sizeof(*timer));
            }
        }
        timer_unlock();

        if (expired_count > 0) {
            timer_begin_ringing();
            for (size_t i = 0; i < expired_count; i++) {
                ESP_LOGW(TAG, "timer expired locally id=%s name=%s", expired[i].id, expired[i].name);
                timer_emit_event("expired", &expired[i]);
            }
        }
        for (size_t i = 0; i < auto_stopped_count; i++) {
            auto_stopped[i].active = false;
            auto_stopped[i].ringing = false;
            timer_emit_event("auto_stopped", &auto_stopped[i]);
        }
        if (auto_stopped_count > 0) {
            timer_finish_ringing_if_idle(true);
        }
        if (timer_any_ringing()) {
            // Recover the tiny handoff window where one alarm task exits just
            // as a different timer begins ringing.
            timer_start_alarm_task();
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

static uint16_t json_u16_clamped(const cJSON *item, uint16_t fallback, uint16_t maximum)
{
    if (!cJSON_IsNumber(item)) {
        return fallback;
    }
    if (item->valuedouble <= 0.0) {
        return 0;
    }
    if (item->valuedouble >= maximum) {
        return maximum;
    }
    return (uint16_t)item->valuedouble;
}

static int64_t json_i64(const cJSON *item, int64_t fallback)
{
    return cJSON_IsNumber(item) ? (int64_t)item->valuedouble : fallback;
}

static int32_t json_i32_clamped(
    const cJSON *item,
    int32_t fallback,
    int32_t minimum,
    int32_t maximum
)
{
    if (!cJSON_IsNumber(item)) {
        return fallback;
    }
    if (item->valuedouble <= minimum) {
        return minimum;
    }
    if (item->valuedouble >= maximum) {
        return maximum;
    }
    return (int32_t)item->valuedouble;
}

static tater_playback_channel_t media_channel_from_json(const cJSON *item)
{
    const char *value =
        cJSON_IsString(item) && item->valuestring ? item->valuestring : "stereo";
    if (strcasecmp(value, "left") == 0) {
        return TATER_PLAYBACK_CHANNEL_LEFT;
    }
    if (strcasecmp(value, "right") == 0) {
        return TATER_PLAYBACK_CHANNEL_RIGHT;
    }
    if (
        strcasecmp(value, "mono") == 0
        || strcasecmp(value, "center") == 0
    ) {
        return TATER_PLAYBACK_CHANNEL_MONO;
    }
    return TATER_PLAYBACK_CHANNEL_STEREO;
}

static void send_simple_result(
    const char *type,
    const char *reply_to,
    bool ok,
    const char *error
)
{
    cJSON *root = new_envelope(type);
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "reply_to", reply_to ? reply_to : "");
    cJSON_AddBoolToObject(payload, "ok", ok);
    if (error && error[0]) {
        cJSON_AddStringToObject(payload, "error", error);
    }
    send_json(root);
}

static void send_hello(void)
{
    s_hello_acked = false;
    s_last_hello_us = esp_timer_get_time();
    cJSON *root = new_envelope("hello");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "device_id", s_device_id);
    cJSON_AddStringToObject(payload, "hardware_id", s_hardware_id);
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
    cJSON_AddBoolToObject(caps, "wake_during_playback", TATER_CAP_WAKE_DURING_PLAYBACK);
    cJSON_AddBoolToObject(caps, "tool_call_mode", true);
    cJSON_AddBoolToObject(caps, "timers", true);
    cJSON_AddBoolToObject(caps, "ota", true);
    cJSON_AddBoolToObject(caps, "xmos", TATER_CAP_XMOS);
    cJSON_AddBoolToObject(caps, "aec", true);
    cJSON_AddBoolToObject(caps, "audio_scenes", true);
    cJSON_AddBoolToObject(caps, "audio_ducking", true);
    cJSON_AddBoolToObject(caps, "looping_background_audio", true);
    cJSON_AddBoolToObject(caps, "persistent_media_sessions", true);
    cJSON_AddBoolToObject(caps, "tts_overlays", true);
    cJSON_AddBoolToObject(caps, "synchronized_media_sessions", true);
    cJSON_AddBoolToObject(caps, "stereo_channel_selection", true);
    cJSON_AddBoolToObject(caps, "media_playhead_telemetry", true);
    cJSON_AddBoolToObject(caps, "media_render_clock", true);
    cJSON_AddBoolToObject(caps, "media_drift_correction", true);
    cJSON_AddBoolToObject(caps, "media_rate_slew", true);
    cJSON_AddBoolToObject(caps, "media_underrun_recovery", true);
    cJSON_AddBoolToObject(caps, "media_session_volume", true);
    cJSON_AddBoolToObject(caps, "media_session_start_position", true);
    cJSON_AddBoolToObject(caps, "synchronized_tts_overlays", true);
    cJSON_AddNumberToObject(caps, "media_sample_rate_hz", TATER_SPK_SAMPLE_RATE);
    cJSON_AddNumberToObject(
        caps,
        "media_output_latency_frames",
        TATER_MEDIA_RENDER_LATENCY_FRAMES
    );
    cJSON_AddNumberToObject(caps, "audio_scene_version", 1);
    cJSON_AddNumberToObject(caps, "audio_session_version", 4);
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
    const cJSON *id_item = cJSON_GetObjectItem(root, "id");
    const cJSON *payload = cJSON_GetObjectItem(root, "payload");
    const char *type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    const char *request_id = cJSON_IsString(id_item) && id_item->valuestring ? id_item->valuestring : "";
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
    } else if (strcmp(type, "wake.verify.result") == 0 && cJSON_IsObject(payload)) {
        const cJSON *request_item = cJSON_GetObjectItem(payload, "request_id");
        const cJSON *accepted_item = cJSON_GetObjectItem(payload, "accepted");
        const cJSON *available_item = cJSON_GetObjectItem(payload, "available");
        const cJSON *reason_item = cJSON_GetObjectItem(payload, "reason");
        uint32_t request_id = cJSON_IsNumber(request_item)
            ? (uint32_t)request_item->valuedouble
            : 0;
        bool accepted = json_truthy(accepted_item);
        bool fail_open = cJSON_IsBool(available_item) && !cJSON_IsTrue(available_item);
        const char *reason = cJSON_IsString(reason_item) && reason_item->valuestring
            ? reason_item->valuestring
            : "";
        tater_wake_engine_verification_result(request_id, accepted, fail_open, reason);
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
    } else if (strcmp(type, "audio.clock.sync") == 0 && cJSON_IsObject(payload)) {
        int64_t satellite_receive_us = esp_timer_get_time();
        const cJSON *server_send_item = cJSON_GetObjectItem(payload, "server_send_us");
        cJSON *response = new_envelope("audio.clock.sync.result");
        cJSON *response_payload = cJSON_GetObjectItem(response, "payload");
        cJSON_AddStringToObject(response_payload, "reply_to", request_id);
        cJSON_AddBoolToObject(response_payload, "ok", true);
        cJSON_AddNumberToObject(
            response_payload,
            "server_send_us",
            (double)json_i64(server_send_item, 0)
        );
        cJSON_AddNumberToObject(
            response_payload,
            "satellite_receive_us",
            (double)satellite_receive_us
        );
        cJSON_AddNumberToObject(
            response_payload,
            "satellite_send_us",
            (double)esp_timer_get_time()
        );
        send_json(response);
    } else if (
        (
            strcmp(type, "media.session.start") == 0
            || strcmp(type, "media.session.prepare") == 0
        )
        && cJSON_IsObject(payload)
    ) {
        bool prepare = strcmp(type, "media.session.prepare") == 0;
        const cJSON *session_id_item = cJSON_GetObjectItem(payload, "session_id");
        const cJSON *group_id_item = cJSON_GetObjectItem(payload, "group_id");
        const cJSON *media = cJSON_GetObjectItem(payload, "media");
        const cJSON *routing = cJSON_GetObjectItem(payload, "routing");
        const cJSON *url_item = cJSON_IsObject(media)
            ? cJSON_GetObjectItem(media, "url")
            : cJSON_GetObjectItem(payload, "url");
        const cJSON *volume_item = cJSON_IsObject(media)
            ? cJSON_GetObjectItem(media, "volume_percent")
            : cJSON_GetObjectItem(payload, "volume_percent");
        const cJSON *start_position_item = cJSON_IsObject(media)
            ? cJSON_GetObjectItem(media, "start_position_ms")
            : cJSON_GetObjectItem(payload, "start_position_ms");
        const cJSON *loop_item = cJSON_IsObject(media)
            ? cJSON_GetObjectItem(media, "loop")
            : cJSON_GetObjectItem(payload, "loop");
        const cJSON *content_type_item = cJSON_IsObject(media)
            ? cJSON_GetObjectItem(media, "content_type")
            : cJSON_GetObjectItem(payload, "content_type");
        const cJSON *channel_item = cJSON_IsObject(routing)
            ? cJSON_GetObjectItem(routing, "channel")
            : cJSON_GetObjectItem(payload, "channel");
        const cJSON *visual_mode_item = cJSON_GetObjectItem(payload, "visual_mode");
        const cJSON *state_after_item = cJSON_GetObjectItem(payload, "state_after");
        const char *session_id =
            cJSON_IsString(session_id_item) && session_id_item->valuestring
            ? session_id_item->valuestring
            : request_id;
        const char *group_id =
            cJSON_IsString(group_id_item) && group_id_item->valuestring
            ? group_id_item->valuestring
            : "";
        const char *url =
            cJSON_IsString(url_item) && url_item->valuestring
            ? url_item->valuestring
            : "";
        const char *content_type =
            cJSON_IsString(content_type_item) && content_type_item->valuestring
            ? content_type_item->valuestring
            : "";
        const char *visual_mode =
            cJSON_IsString(visual_mode_item) && visual_mode_item->valuestring
            ? visual_mode_item->valuestring
            : "";
        const char *state_after =
            cJSON_IsString(state_after_item) && state_after_item->valuestring
            ? state_after_item->valuestring
            : "";
        bool loop = cJSON_IsBool(loop_item) ? cJSON_IsTrue(loop_item) : json_truthy(loop_item);
        bool transient_tts =
            strcasecmp(content_type, "tts") == 0
            || strcasecmp(content_type, "speech") == 0
            || strcasecmp(content_type, "announcement") == 0;
        bool complete_visual_state =
            transient_tts
            || strcasecmp(visual_mode, "speaking") == 0
            || strcasecmp(visual_mode, "tool_call") == 0;
        bool tool_playback =
            strcasecmp(visual_mode, "tool_call") == 0
            || strcasecmp(state_after, "tool_call") == 0;
        int64_t raw_start_position_ms = json_i64(start_position_item, 0);
        uint32_t start_position_ms = raw_start_position_ms <= 0
            ? 0
            : (raw_start_position_ms > UINT32_MAX
                ? UINT32_MAX
                : (uint32_t)raw_start_position_ms);

        tater_playback_media_session_t media_session = {
            .session_id = session_id,
            .group_id = group_id,
            .prepare_reply_to = prepare ? request_id : "",
            .url = url,
            .volume_percent = (uint8_t)json_u16_clamped(volume_item, 100, 100),
            .start_position_ms = start_position_ms,
            .channel = media_channel_from_json(channel_item),
            .loop = loop,
            .prepare = prepare,
            .complete_visual_state = complete_visual_state,
            .tool_visual_state = tool_playback,
        };
        esp_err_t media_err = tater_playback_start_media_session(&media_session);
        if (media_err != ESP_OK) {
            ESP_LOGE(TAG, "media session start failed: %s", esp_err_to_name(media_err));
            if (prepare) {
                tater_protocol_send_media_session_ready(
                    session_id,
                    group_id,
                    request_id,
                    false,
                    0
                );
            }
            tater_protocol_send_media_session_finished(
                session_id,
                false,
                complete_visual_state
            );
        } else {
            ESP_LOGI(
                TAG,
                "media session queued id=%s group=%s channel=%d loop=%d prepare=%d visual=%d",
                session_id && session_id[0] ? session_id : "-",
                group_id && group_id[0] ? group_id : "-",
                (int)media_session.channel,
                loop,
                prepare,
                complete_visual_state
            );
        }
    } else if (strcmp(type, "media.session.commit") == 0 && cJSON_IsObject(payload)) {
        const cJSON *session_id_item = cJSON_GetObjectItem(payload, "session_id");
        const cJSON *start_at_item = cJSON_GetObjectItem(payload, "start_at_us");
        const char *session_id =
            cJSON_IsString(session_id_item) && session_id_item->valuestring
            ? session_id_item->valuestring
            : "";
        esp_err_t commit_err = tater_playback_commit_media_session(
            session_id,
            json_i64(start_at_item, 0)
        );
        send_simple_result(
            "media.session.commit.result",
            request_id,
            commit_err == ESP_OK,
            commit_err == ESP_OK ? "" : esp_err_to_name(commit_err)
        );
    } else if (strcmp(type, "media.session.volume") == 0 && cJSON_IsObject(payload)) {
        const cJSON *session_id_item = cJSON_GetObjectItem(payload, "session_id");
        const cJSON *volume_item = cJSON_GetObjectItem(payload, "volume_percent");
        const char *session_id =
            cJSON_IsString(session_id_item) && session_id_item->valuestring
            ? session_id_item->valuestring
            : "";
        uint8_t volume_percent =
            (uint8_t)json_u16_clamped(volume_item, 100, 100);
        esp_err_t volume_err = tater_playback_set_media_session_volume(
            session_id,
            volume_percent
        );
        send_simple_result(
            "media.session.volume.result",
            request_id,
            volume_err == ESP_OK,
            volume_err == ESP_OK ? "" : esp_err_to_name(volume_err)
        );
    } else if (strcmp(type, "media.session.adjust") == 0 && cJSON_IsObject(payload)) {
        const cJSON *session_id_item = cJSON_GetObjectItem(payload, "session_id");
        const cJSON *correction_item = cJSON_GetObjectItem(payload, "correction_frames");
        const cJSON *mode_item = cJSON_GetObjectItem(payload, "mode");
        const cJSON *settle_item = cJSON_GetObjectItem(payload, "settle_ms");
        const char *session_id =
            cJSON_IsString(session_id_item) && session_id_item->valuestring
            ? session_id_item->valuestring
            : "";
        const char *mode =
            cJSON_IsString(mode_item) && mode_item->valuestring
            ? mode_item->valuestring
            : "slew";
        int32_t correction_frames =
            json_i32_clamped(correction_item, 0, -480, 480);
        uint32_t settle_ms = json_u16_clamped(settle_item, 1000, 10000);
        esp_err_t adjust_err = tater_playback_adjust_media_session(
            session_id,
            correction_frames,
            mode,
            settle_ms
        );
        send_simple_result(
            "media.session.adjust.result",
            request_id,
            adjust_err == ESP_OK,
            adjust_err == ESP_OK ? "" : esp_err_to_name(adjust_err)
        );
    } else if (strcmp(type, "media.session.stop") == 0) {
        ESP_LOGI(TAG, "media.session.stop");
        tater_playback_stop();
    } else if (strcmp(type, "audio.overlay.start") == 0 && cJSON_IsObject(payload)) {
        const cJSON *overlay_id_item = cJSON_GetObjectItem(payload, "overlay_id");
        const cJSON *foreground = cJSON_GetObjectItem(payload, "foreground");
        const cJSON *ducking = cJSON_GetObjectItem(payload, "ducking");
        const cJSON *url_item = cJSON_IsObject(foreground)
            ? cJSON_GetObjectItem(foreground, "url")
            : cJSON_GetObjectItem(payload, "url");
        const cJSON *volume_item = cJSON_IsObject(foreground)
            ? cJSON_GetObjectItem(foreground, "volume_percent")
            : cJSON_GetObjectItem(payload, "volume_percent");
        const cJSON *duck_target_item = cJSON_IsObject(ducking)
            ? cJSON_GetObjectItem(ducking, "target_percent")
            : NULL;
        const cJSON *duck_attack_item = cJSON_IsObject(ducking)
            ? cJSON_GetObjectItem(ducking, "attack_ms")
            : NULL;
        const cJSON *duck_release_item = cJSON_IsObject(ducking)
            ? cJSON_GetObjectItem(ducking, "release_ms")
            : NULL;
        const cJSON *foreground_kind_item = cJSON_IsObject(foreground)
            ? cJSON_GetObjectItem(foreground, "kind")
            : NULL;
        const cJSON *visual_mode_item = cJSON_GetObjectItem(payload, "visual_mode");
        const cJSON *state_after_item = cJSON_GetObjectItem(payload, "state_after");
        const cJSON *start_at_item = cJSON_GetObjectItem(payload, "start_at_us");
        const char *overlay_id =
            cJSON_IsString(overlay_id_item) && overlay_id_item->valuestring
            ? overlay_id_item->valuestring
            : request_id;
        const char *url =
            cJSON_IsString(url_item) && url_item->valuestring
            ? url_item->valuestring
            : "";
        const char *foreground_kind =
            cJSON_IsString(foreground_kind_item) && foreground_kind_item->valuestring
            ? foreground_kind_item->valuestring
            : "tts";
        const char *visual_mode =
            cJSON_IsString(visual_mode_item) && visual_mode_item->valuestring
            ? visual_mode_item->valuestring
            : "";
        const char *state_after =
            cJSON_IsString(state_after_item) && state_after_item->valuestring
            ? state_after_item->valuestring
            : "";
        bool tool_playback = strcmp(foreground_kind, "tool") == 0
            || strcmp(foreground_kind, "tool_progress") == 0
            || strcmp(visual_mode, "tool_call") == 0
            || strcmp(state_after, "tool_call") == 0;

        if (state_after[0]) {
            s_playback_return_state = parse_state(state_after);
            s_playback_return_armed = true;
        } else if (tool_playback) {
            s_playback_return_state = TATER_STATE_TOOL_CALL;
            s_playback_return_armed = true;
        } else {
            s_playback_return_armed = false;
            s_playback_return_state = TATER_STATE_IDLE;
        }

        tater_playback_overlay_t overlay = {
            .overlay_id = overlay_id,
            .foreground_url = url,
            .foreground_volume_percent =
                (uint8_t)json_u16_clamped(volume_item, 100, 100),
            .ducking_target_percent =
                (uint8_t)json_u16_clamped(duck_target_item, 20, 100),
            .ducking_attack_ms = json_u16_clamped(duck_attack_item, 150, 10000),
            .ducking_release_ms = json_u16_clamped(duck_release_item, 350, 10000),
            .start_at_us = json_i64(start_at_item, 0),
        };
        mark_playback_visual_active();
        emit_state(
            tool_playback ? TATER_STATE_TOOL_CALL : TATER_STATE_SPEAKING,
            tool_playback ? "tool audio overlay" : "audio overlay"
        );
        if (!tater_playback_media_session_active() && s_play_url_cb && url[0]) {
            ESP_LOGW(TAG, "audio overlay has no active media session; using standalone playback");
            s_play_url_cb(url, tool_playback ? TATER_STATE_TOOL_CALL : TATER_STATE_SPEAKING);
        } else {
            esp_err_t overlay_err = tater_playback_play_overlay(&overlay);
            if (overlay_err != ESP_OK) {
                ESP_LOGE(TAG, "audio overlay start failed: %s", esp_err_to_name(overlay_err));
                tater_protocol_send_audio_overlay_finished(overlay_id, false);
            }
        }
    } else if (strcmp(type, "audio.scene.start") == 0 && cJSON_IsObject(payload)) {
        const cJSON *scene_id_item = cJSON_GetObjectItem(payload, "scene_id");
        const cJSON *foreground = cJSON_GetObjectItem(payload, "foreground");
        const cJSON *background = cJSON_GetObjectItem(payload, "background");
        const cJSON *ducking = cJSON_GetObjectItem(payload, "ducking");
        const cJSON *finish = cJSON_GetObjectItem(payload, "finish");
        const cJSON *visual_mode_item = cJSON_GetObjectItem(payload, "visual_mode");
        const cJSON *state_after_item = cJSON_GetObjectItem(payload, "state_after");
        const cJSON *foreground_url_item = cJSON_IsObject(foreground)
            ? cJSON_GetObjectItem(foreground, "url")
            : NULL;
        const cJSON *foreground_kind_item = cJSON_IsObject(foreground)
            ? cJSON_GetObjectItem(foreground, "kind")
            : NULL;
        const cJSON *foreground_volume_item = cJSON_IsObject(foreground)
            ? cJSON_GetObjectItem(foreground, "volume_percent")
            : NULL;
        const cJSON *background_url_item = cJSON_IsObject(background)
            ? cJSON_GetObjectItem(background, "url")
            : NULL;
        const cJSON *background_loop_item = cJSON_IsObject(background)
            ? cJSON_GetObjectItem(background, "loop")
            : NULL;
        const cJSON *background_volume_item = cJSON_IsObject(background)
            ? cJSON_GetObjectItem(background, "volume_percent")
            : NULL;
        const cJSON *duck_target_item = cJSON_IsObject(ducking)
            ? cJSON_GetObjectItem(ducking, "target_percent")
            : NULL;
        const cJSON *duck_attack_item = cJSON_IsObject(ducking)
            ? cJSON_GetObjectItem(ducking, "attack_ms")
            : NULL;
        const cJSON *duck_release_item = cJSON_IsObject(ducking)
            ? cJSON_GetObjectItem(ducking, "release_ms")
            : NULL;
        const cJSON *fade_item = cJSON_IsObject(finish)
            ? cJSON_GetObjectItem(finish, "fade_ms")
            : NULL;

        const char *scene_id = cJSON_IsString(scene_id_item) && scene_id_item->valuestring
            ? scene_id_item->valuestring
            : request_id;
        const char *foreground_url =
            cJSON_IsString(foreground_url_item) && foreground_url_item->valuestring
            ? foreground_url_item->valuestring
            : "";
        const char *foreground_kind =
            cJSON_IsString(foreground_kind_item) && foreground_kind_item->valuestring
            ? foreground_kind_item->valuestring
            : "tts";
        const char *background_url =
            cJSON_IsString(background_url_item) && background_url_item->valuestring
            ? background_url_item->valuestring
            : "";
        const char *visual_mode =
            cJSON_IsString(visual_mode_item) && visual_mode_item->valuestring
            ? visual_mode_item->valuestring
            : "";
        const char *state_after =
            cJSON_IsString(state_after_item) && state_after_item->valuestring
            ? state_after_item->valuestring
            : "";
        bool tool_playback = strcmp(foreground_kind, "tool") == 0
            || strcmp(foreground_kind, "tool_progress") == 0
            || strcmp(visual_mode, "tool_call") == 0
            || strcmp(state_after, "tool_call") == 0;
        tater_state_t visual_state = tool_playback ? TATER_STATE_TOOL_CALL : TATER_STATE_SPEAKING;

        if (!foreground_url[0]) {
            ESP_LOGW(TAG, "audio.scene.start rejected: foreground url missing");
            tater_protocol_send_audio_scene_finished(scene_id, false);
        } else {
            if (state_after[0]) {
                s_playback_return_state = parse_state(state_after);
                s_playback_return_armed = true;
            } else if (tool_playback) {
                s_playback_return_state = TATER_STATE_TOOL_CALL;
                s_playback_return_armed = true;
            } else {
                s_playback_return_armed = false;
                s_playback_return_state = TATER_STATE_IDLE;
            }

            bool background_loop = true;
            if (cJSON_IsBool(background_loop_item)) {
                background_loop = cJSON_IsTrue(background_loop_item);
            } else if (background_loop_item) {
                background_loop = json_truthy(background_loop_item);
            }
            tater_playback_scene_t scene = {
                .scene_id = scene_id,
                .foreground_url = foreground_url,
                .background_url = background_url,
                .foreground_volume_percent =
                    (uint8_t)json_u16_clamped(foreground_volume_item, 100, 100),
                .background_volume_percent =
                    (uint8_t)json_u16_clamped(background_volume_item, 100, 100),
                .ducking_target_percent =
                    (uint8_t)json_u16_clamped(duck_target_item, 20, 100),
                .ducking_attack_ms = json_u16_clamped(duck_attack_item, 150, 10000),
                .ducking_release_ms = json_u16_clamped(duck_release_item, 350, 10000),
                .background_fade_out_ms = json_u16_clamped(fade_item, 350, 10000),
                .background_loop = background_loop,
            };

            ESP_LOGI(
                TAG,
                "audio.scene.start id=%s kind=%s background=%d loop=%d duck=%u%%",
                scene_id && scene_id[0] ? scene_id : "-",
                foreground_kind,
                background_url[0] != '\0',
                background_loop,
                scene.ducking_target_percent
            );
            if (!tool_playback) {
                s_tool_visual_hold = false;
            }
            mark_playback_visual_active();
            emit_state(visual_state, tool_playback ? "tool audio scene" : "audio scene");
            esp_err_t scene_err = tater_playback_play_scene(&scene);
            if (scene_err != ESP_OK) {
                ESP_LOGE(TAG, "audio scene start failed: %s", esp_err_to_name(scene_err));
                tater_protocol_send_audio_scene_finished(scene_id, false);
            }
        }
    } else if (strcmp(type, "audio.scene.stop") == 0) {
        ESP_LOGI(TAG, "audio.scene.stop");
        tater_playback_stop();
    } else if (strcmp(type, "play.url") == 0 && cJSON_IsObject(payload)) {
        const cJSON *url_item = cJSON_GetObjectItem(payload, "url");
        const cJSON *tts_kind_item = cJSON_GetObjectItem(payload, "tts_kind");
        const cJSON *visual_mode_item = cJSON_GetObjectItem(payload, "visual_mode");
        const cJSON *state_after_item = cJSON_GetObjectItem(payload, "state_after");
        const cJSON *ducking = cJSON_GetObjectItem(payload, "ducking");
        const cJSON *duck_target_item = cJSON_IsObject(ducking)
            ? cJSON_GetObjectItem(ducking, "target_percent")
            : NULL;
        const cJSON *duck_attack_item = cJSON_IsObject(ducking)
            ? cJSON_GetObjectItem(ducking, "attack_ms")
            : NULL;
        const cJSON *duck_release_item = cJSON_IsObject(ducking)
            ? cJSON_GetObjectItem(ducking, "release_ms")
            : NULL;
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
            bool overlay_started = false;
            bool media_active = tater_playback_media_session_active();
            if (media_active) {
                tater_playback_overlay_t overlay = {
                    .overlay_id = request_id,
                    .foreground_url = url_item->valuestring,
                    .foreground_volume_percent = 100,
                    .ducking_target_percent =
                        (uint8_t)json_u16_clamped(duck_target_item, 20, 100),
                    .ducking_attack_ms =
                        json_u16_clamped(duck_attack_item, 150, 10000),
                    .ducking_release_ms =
                        json_u16_clamped(duck_release_item, 350, 10000),
                };
                esp_err_t overlay_err = tater_playback_play_overlay(&overlay);
                if (overlay_err == ESP_OK) {
                    overlay_started = true;
                    ESP_LOGI(TAG, "play.url promoted to media overlay id=%s", request_id);
                } else {
                    ESP_LOGW(
                        TAG,
                        "play.url overlay unavailable (%s); preserving media session",
                        esp_err_to_name(overlay_err)
                    );
                    tater_protocol_send_audio_overlay_finished(request_id, false);
                    overlay_started = true;
                }
            }
            if (!overlay_started) {
                s_play_url_cb(url_item->valuestring, visual_state);
            }
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
    } else if ((strcmp(type, "timer.start") == 0 || strcmp(type, "timer.arm") == 0) && cJSON_IsObject(payload)) {
        timer_start_from_payload(payload, request_id, strcmp(type, "timer.arm") == 0);
    } else if ((strcmp(type, "timer.list") == 0 || strcmp(type, "timer.status") == 0)) {
        timer_send_list_result(request_id);
    } else if (strcmp(type, "timer.cancel") == 0 && cJSON_IsObject(payload)) {
        timer_cancel_from_payload(payload, request_id, "cancelled");
    } else if (strcmp(type, "timer.snooze") == 0 && cJSON_IsObject(payload)) {
        timer_snooze_from_payload(payload, request_id);
    } else if (strcmp(type, "timer.alarm") == 0 && cJSON_IsObject(payload)) {
        timer_force_alarm_from_payload(payload);
    } else if (strcmp(type, "timer.clear") == 0) {
        cJSON *empty = cJSON_CreateObject();
        timer_cancel_from_payload(cJSON_IsObject(payload) ? payload : empty, request_id, "cleared");
        cJSON_Delete(empty);
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
        s_json_send_failure_streak = 0;
        s_link_down_started_us = 0;
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
    char normalized_server_url[sizeof(s_config.server_url)];
    if (tater_server_normalize_base_url(
            s_config.server_url,
            normalized_server_url,
            sizeof(normalized_server_url)
        )) {
        strlcpy(s_config.server_url, normalized_server_url, sizeof(s_config.server_url));
    }
    s_state_cb = state_cb;
    s_play_url_cb = play_url_cb;
    s_play_tone_cb = play_tone_cb;
    s_ota_url_cb = ota_url_cb;
    tater_live_settings_init_defaults();
    s_send_lock = xSemaphoreCreateMutex();
    s_timer_lock = xSemaphoreCreateMutex();
    esp_err_t audio_tx_err = audio_tx_init();
    if (audio_tx_err != ESP_OK) {
        ESP_LOGE(TAG, "audio tx queue init failed: %s", esp_err_to_name(audio_tx_err));
    }
    esp_err_t media_tx_err = media_tx_init();
    if (media_tx_err != ESP_OK) {
        ESP_LOGE(TAG, "media telemetry queue init failed: %s", esp_err_to_name(media_tx_err));
    }
    build_device_identity();
    build_ws_url();
    if (strlen(s_config.token) > 0) {
        snprintf(s_auth_header, sizeof(s_auth_header), "X-Tater-Token: %s\r\n", s_config.token);
    }
}

void tater_protocol_start(void)
{
    audio_tx_start_task();
    media_tx_start_task();

    esp_err_t websocket_start_err = ESP_ERR_INVALID_ARG;
    if (s_ws_url[0]) {
        websocket_start_err = create_websocket_client();
        if (websocket_start_err == ESP_OK) {
            websocket_start_err = esp_websocket_client_start(s_client);
        }
    }
    if (websocket_start_err != ESP_OK) {
        ESP_LOGE(TAG, "websocket startup failed: %s", esp_err_to_name(websocket_start_err));
        emit_state(TATER_STATE_ERROR, s_ws_url[0] ? "websocket startup failed" : "invalid server URL");
    }

    if (s_ws_url[0]) {
        BaseType_t task_ok = xTaskCreate(reconnect_watchdog_task, "tater_ws_reconnect", 4096, NULL, 4, NULL);
        if (task_ok != pdPASS) {
            ESP_LOGE(TAG, "websocket reconnect watchdog task create failed");
        }
    }
    if (!s_timer_monitor_task) {
        BaseType_t timer_task_ok = xTaskCreate(timer_monitor_task, "tater_timer", 5120, NULL, 4, &s_timer_monitor_task);
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
    return timer_any_ringing();
}

bool tater_protocol_timer_is_active(void)
{
    return timer_any_active();
}

void tater_protocol_timer_stop_from_device(void)
{
    if (!timer_any_ringing()) {
        return;
    }
    ESP_LOGI(TAG, "timer stopped from device button");
    cJSON *empty = cJSON_CreateObject();
    if (empty) {
        timer_cancel_from_payload(empty, "", "stopped");
        cJSON_Delete(empty);
    }
}

bool tater_protocol_can_detect_timer_stop(void)
{
    if (!timer_any_ringing() || s_voice_active || tater_ota_is_running()) {
        return false;
    }
    const tater_live_settings_t *settings = tater_live_settings_get();
    return !settings || !settings->muted;
}

bool tater_protocol_can_start_local_wake(void)
{
    if (!websocket_ready() || s_voice_active || timer_any_ringing()) {
        return false;
    }
    const tater_live_settings_t *settings = tater_live_settings_get();
    if (settings && settings->muted) {
        return false;
    }
#if !TATER_CAP_WAKE_DURING_PLAYBACK
    if (tater_playback_is_playing()) {
        return settings && settings->barge_in_enabled;
    }
#endif
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
    cJSON *timer_rows = cJSON_CreateArray();
    timer_lock();
    int active_timers = timer_active_count_locked();
    int ringing_timers = timer_ringing_count_locked();
    cJSON_AddBoolToObject(timer, "active", active_timers > 0);
    cJSON_AddBoolToObject(timer, "ringing", ringing_timers > 0);
    cJSON_AddNumberToObject(timer, "count", active_timers);
    cJSON_AddNumberToObject(timer, "ringing_count", ringing_timers);
    for (size_t i = 0; i < TATER_MAX_LOCAL_TIMERS; i++) {
        if (!s_timers[i].active) {
            continue;
        }
        cJSON_AddItemToArray(timer_rows, timer_json(&s_timers[i]));
    }
    timer_unlock();
    cJSON_AddItemToObject(timer, "timers", timer_rows);
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
    cJSON_AddBoolToObject(transport, "auto_reconnect", true);
    cJSON_AddBoolToObject(transport, "lifecycle_restart_active", s_ws_lifecycle_restart);
    cJSON_AddNumberToObject(transport, "lifecycle_restarts", s_ws_restart_count);
    cJSON_AddNumberToObject(transport, "lifecycle_restart_failures", s_ws_restart_failures);
    cJSON_AddNumberToObject(transport, "json_send_failure_total", s_json_send_failure_total);
    cJSON_AddNumberToObject(transport, "json_send_failure_streak", s_json_send_failure_streak);
    cJSON_AddNumberToObject(transport, "json_send_failures_tolerated", s_json_send_failures_tolerated);
    cJSON_AddNumberToObject(transport, "last_json_send_result", s_last_json_send_result);
    cJSON_AddStringToObject(
        transport,
        "last_json_send_type",
        s_last_json_send_type[0] ? s_last_json_send_type : ""
    );
    cJSON_AddNumberToObject(
        transport,
        "media_tx_queue_depth",
        s_media_tx_queue ? uxQueueMessagesWaiting(s_media_tx_queue) : 0
    );
    cJSON_AddNumberToObject(transport, "media_tx_high_water", s_media_tx_high_water);
    cJSON_AddNumberToObject(transport, "media_tx_dropped", s_media_tx_dropped);
    cJSON_AddItemToObject(payload, "transport", transport);
    cJSON_AddItemToObject(payload, "reset", reset_diag_json());
#if TATER_BOARD_SAT1
    /*
     * The server already owns and reports the desired live settings.  Echoing
     * their URLs and animation names here pushed Sat1 status frames beyond
     * 4096 bytes, which some websocket paths fail to consume.  Keep only the
     * applied generation for drift diagnostics and leave room for crash and
     * board diagnostics.
     */
    const tater_live_settings_t *applied_settings = tater_live_settings_get();
    if (applied_settings) {
        cJSON_AddNumberToObject(payload, "settings_generation", applied_settings->wake_settings_generation);
    }
#else
    tater_live_settings_add_status(payload);
#endif
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
        cJSON_AddNumberToObject(xmos, "sample_delay_q8", doa.sample_delay_q8);
        cJSON_AddNumberToObject(xmos, "vertical_delay_q8", doa.vertical_delay_q8);
        cJSON_AddNumberToObject(xmos, "angle_index", doa.angle_index);
        cJSON_AddBoolToObject(xmos, "four_mic", doa.four_mic);
        cJSON_AddNumberToObject(xmos, "energy", doa.energy);
        cJSON_AddNumberToObject(xmos, "noise_floor_energy", doa.noise_floor_energy);
        cJSON_AddBoolToObject(xmos, "signal_active", doa.signal_active != 0);
        cJSON_AddNumberToObject(xmos, "control_flags", doa.control_flags);
        cJSON_AddNumberToObject(xmos, "mode_flags", doa.mode_flags);
        cJSON_AddNumberToObject(xmos, "active_mic_mask", doa.active_mic_mask);
        cJSON *mic_energy = cJSON_CreateArray();
        if (mic_energy) {
            for (size_t i = 0; i < 4; i++) {
                cJSON_AddItemToArray(mic_energy, cJSON_CreateNumber(doa.mic_energy[i]));
            }
            cJSON_AddItemToObject(xmos, "mic_energy", mic_energy);
        }
        cJSON *mic_health = cJSON_CreateArray();
        cJSON *mic_gain = cJSON_CreateArray();
        cJSON *beam_delay = cJSON_CreateArray();
        if (mic_health && mic_gain && beam_delay) {
            for (size_t i = 0; i < 4; i++) {
                cJSON_AddItemToArray(mic_health, cJSON_CreateNumber(doa.mic_health_flags[i]));
                cJSON_AddItemToArray(mic_gain, cJSON_CreateNumber(doa.mic_gain_q15[i]));
                cJSON_AddItemToArray(beam_delay, cJSON_CreateNumber(doa.beam_delay_q8[i]));
            }
            cJSON_AddItemToObject(xmos, "mic_health", mic_health);
            cJSON_AddItemToObject(xmos, "mic_gain_q15", mic_gain);
            cJSON_AddItemToObject(xmos, "beam_delay_q8", beam_delay);
        } else {
            cJSON_Delete(mic_health);
            cJSON_Delete(mic_gain);
            cJSON_Delete(beam_delay);
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
#if TATER_BOARD_SAT1
    tater_audio_power_status_t power_status = {0};
    if (tater_audio_power_status_snapshot(&power_status)) {
        cJSON *power = cJSON_CreateObject();
        cJSON_AddStringToObject(power, "state", power_status.state);
        cJSON_AddBoolToObject(power, "controller_present", power_status.controller_present);
        cJSON_AddBoolToObject(power, "attached", power_status.attached);
        cJSON_AddBoolToObject(power, "explicit_contract", power_status.explicit_contract);
        cJSON_AddBoolToObject(power, "negotiation_failed", power_status.negotiation_failed);
        cJSON_AddNumberToObject(power, "controller_device_id", power_status.device_id);
        cJSON_AddNumberToObject(power, "cc_pin", power_status.cc_pin);
        cJSON_AddNumberToObject(power, "source_pdo_count", power_status.source_pdo_count);
        cJSON_AddNumberToObject(power, "requested_voltage_mv", power_status.requested_voltage_mv);
        cJSON_AddNumberToObject(power, "contract_voltage_mv", power_status.contract_voltage_mv);
        cJSON_AddNumberToObject(power, "contract_current_ma", power_status.contract_current_ma);
        cJSON_AddNumberToObject(power, "tas2780_power_mode", power_status.tas_power_mode);
        cJSON_AddItemToObject(payload, "power_delivery", power);
    }
#endif
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
            if (create_transient_task(
                    continued_reopen_watchdog_task,
                    "reopen_watchdog",
                    3072,
                    request,
                    4,
                    &request->task_with_caps
                ) != pdPASS) {
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

    bool ack_grace_expired = false;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    if (s_voice_start_pending) {
        int64_t pending_ms = s_voice_started_us > 0
            ? (esp_timer_get_time() - s_voice_started_us) / 1000
            : 0;
        if (pending_ms < TATER_VOICE_START_ACK_GRACE_MS) {
            buffer_audio_preroll_locked(pcm, sample_count);
            xSemaphoreGive(s_send_lock);
            return;
        }

        /*
         * A voice.start text frame and the following binary frames share one
         * ordered websocket connection, so audio cannot overtake voice.start.
         * Do not let a slow or lost acknowledgement leave the microphone
         * permanently gated: release the buffered audio after a short grace
         * period and still honor a later negative acknowledgement.
         */
        s_voice_start_pending = false;
        flush_audio_preroll_locked();
        ack_grace_expired = true;
    }
    xSemaphoreGive(s_send_lock);
    if (ack_grace_expired) {
        ESP_LOGW(
            TAG,
            "voice.start ack delayed beyond %u ms; streaming audio",
            (unsigned)TATER_VOICE_START_ACK_GRACE_MS
        );
    }

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

void tater_protocol_send_volume_changed(uint8_t volume_percent)
{
    cJSON *root = new_envelope("settings.changed");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON *settings = cJSON_CreateObject();
    cJSON_AddNumberToObject(
        settings,
        "volume_percent",
        volume_percent > 100 ? 100 : volume_percent
    );
    cJSON_AddItemToObject(payload, "settings", settings);
    cJSON_AddStringToObject(payload, "source", "device");
    send_json(root);
}

static void continued_reopen_watchdog_task(void *arg)
{
    tater_voice_watchdog_args_t *request = (tater_voice_watchdog_args_t *)arg;
    uint32_t generation = request ? request->generation : 0;
    bool task_with_caps = request && request->task_with_caps;
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

    delete_transient_task(task_with_caps);
}

static void continued_reopen_task(void *arg)
{
    tater_reopen_args_t *request = (tater_reopen_args_t *)arg;
    bool task_with_caps = request && request->task_with_caps;
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
            delete_transient_task(task_with_caps);
            return;
        }
        ESP_LOGI(TAG, "continued chat reopening mic conversation_id=%s", conversation_id);
        tater_protocol_start_voice_with_conversation("", "continued_chat", conversation_id);
    }
    delete_transient_task(task_with_caps);
}

static void send_playback_completion(bool ok, bool allow_reopen, bool emit_playback_event)
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

    if (emit_playback_event) {
        cJSON *root = new_envelope("playback.finished");
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        cJSON_AddBoolToObject(payload, "ok", ok);
        send_json(root);
    }

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
            if (create_transient_task(
                    continued_reopen_task,
                    "tater_reopen",
                    4096,
                    request,
                    5,
                    &request->task_with_caps
                ) != pdPASS) {
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

static void finish_media_session_visual(bool ok)
{
    bool return_armed = s_playback_return_armed;
    tater_state_t return_state = s_playback_return_state;
    s_playback_return_armed = false;
    s_playback_return_state = TATER_STATE_IDLE;
    clear_playback_visual_active();

    tater_state_t next_state = return_armed ? return_state : TATER_STATE_IDLE;
    if (!websocket_ready()) {
        next_state = TATER_STATE_DISCONNECTED;
    }
    emit_state(
        next_state,
        return_armed ? "playback return" : (ok ? "playback finished" : "playback stopped")
    );
}

void tater_protocol_send_playback_finished_status(bool ok, bool allow_reopen)
{
    send_playback_completion(ok, allow_reopen, true);
}

void tater_protocol_send_playback_finished(void)
{
    tater_protocol_send_playback_finished_status(true, true);
}

void tater_protocol_send_audio_scene_finished(const char *scene_id, bool ok)
{
    cJSON *root = new_envelope("audio.scene.finished");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "scene_id", scene_id ? scene_id : "");
    cJSON_AddBoolToObject(payload, "ok", ok);
    send_json(root);
    tater_protocol_send_playback_finished_status(ok, ok);
}

void tater_protocol_send_audio_overlay_started(const char *overlay_id)
{
    cJSON *root = new_envelope("audio.overlay.started");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "overlay_id", overlay_id ? overlay_id : "");
    send_json(root);
}

void tater_protocol_send_audio_overlay_finished(const char *overlay_id, bool ok)
{
    cJSON *root = new_envelope("audio.overlay.finished");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "overlay_id", overlay_id ? overlay_id : "");
    cJSON_AddBoolToObject(payload, "ok", ok);
    send_json(root);
    send_playback_completion(ok, ok, false);
}

void tater_protocol_start_media_session_visual(bool tool_playback)
{
    if (tool_playback) {
        s_playback_return_state = TATER_STATE_TOOL_CALL;
        s_playback_return_armed = true;
    } else {
        s_playback_return_state = TATER_STATE_IDLE;
        s_playback_return_armed = false;
        s_tool_visual_hold = false;
    }
    mark_playback_visual_active();
    emit_state(
        tool_playback ? TATER_STATE_TOOL_CALL : TATER_STATE_SPEAKING,
        tool_playback ? "tool media session" : "tts media session"
    );
}

void tater_protocol_send_media_session_started(
    const char *session_id,
    const char *group_id,
    const char *channel,
    int64_t scheduled_start_us,
    int64_t actual_start_us
)
{
    tater_media_tx_event_t event = {
        .kind = TATER_MEDIA_TX_STARTED,
        .payload.started.scheduled_start_us = scheduled_start_us,
        .payload.started.actual_start_us = actual_start_us,
    };
    strlcpy(event.session_id, session_id ? session_id : "", sizeof(event.session_id));
    strlcpy(
        event.payload.started.group_id,
        group_id ? group_id : "",
        sizeof(event.payload.started.group_id)
    );
    strlcpy(
        event.payload.started.channel,
        channel ? channel : "stereo",
        sizeof(event.payload.started.channel)
    );
    (void)media_tx_enqueue(&event);
}

void tater_protocol_send_media_session_finished(
    const char *session_id,
    bool ok,
    bool complete_visual_state
)
{
    tater_media_tx_event_t event = {
        .kind = TATER_MEDIA_TX_FINISHED,
        .payload.finished.ok = ok,
    };
    strlcpy(event.session_id, session_id ? session_id : "", sizeof(event.session_id));
    (void)media_tx_enqueue(&event);
    if (complete_visual_state) {
        finish_media_session_visual(ok);
    }
}

void tater_protocol_send_media_session_ready(
    const char *session_id,
    const char *group_id,
    const char *reply_to,
    bool ok,
    uint32_t buffered_frames
)
{
    cJSON *root = new_envelope("media.session.prepare.result");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "reply_to", reply_to ? reply_to : "");
    cJSON_AddBoolToObject(payload, "ok", ok);
    cJSON_AddStringToObject(payload, "session_id", session_id ? session_id : "");
    cJSON_AddStringToObject(payload, "group_id", group_id ? group_id : "");
    cJSON_AddNumberToObject(payload, "buffered_frames", buffered_frames);
    cJSON_AddNumberToObject(payload, "sample_rate_hz", TATER_SPK_SAMPLE_RATE);
    cJSON_AddNumberToObject(
        payload,
        "output_latency_frames",
        TATER_MEDIA_RENDER_LATENCY_FRAMES
    );
    cJSON_AddNumberToObject(payload, "satellite_time_us", (double)esp_timer_get_time());
    send_json(root);
}

void tater_protocol_send_media_session_playhead(
    const char *session_id,
    const char *group_id,
    const char *channel,
    uint64_t source_frames,
    uint64_t rendered_frames,
    uint64_t output_frames,
    uint32_t output_latency_frames,
    uint32_t buffered_frames,
    int64_t satellite_time_us,
    int64_t scheduled_start_us,
    int32_t correction_frames,
    bool rebuffering,
    uint32_t underrun_events,
    uint32_t rejoin_count,
    uint64_t rejoin_frames
)
{
    tater_media_tx_event_t event = {
        .kind = TATER_MEDIA_TX_PLAYHEAD,
        .payload.playhead.source_frames = source_frames,
        .payload.playhead.rendered_frames = rendered_frames,
        .payload.playhead.output_frames = output_frames,
        .payload.playhead.output_latency_frames = output_latency_frames,
        .payload.playhead.buffered_frames = buffered_frames,
        .payload.playhead.satellite_time_us = satellite_time_us,
        .payload.playhead.scheduled_start_us = scheduled_start_us,
        .payload.playhead.correction_frames = correction_frames,
        .payload.playhead.rebuffering = rebuffering,
        .payload.playhead.underrun_events = underrun_events,
        .payload.playhead.rejoin_count = rejoin_count,
        .payload.playhead.rejoin_frames = rejoin_frames,
    };
    strlcpy(event.session_id, session_id ? session_id : "", sizeof(event.session_id));
    strlcpy(
        event.payload.playhead.group_id,
        group_id ? group_id : "",
        sizeof(event.payload.playhead.group_id)
    );
    strlcpy(
        event.payload.playhead.channel,
        channel ? channel : "stereo",
        sizeof(event.payload.playhead.channel)
    );
    (void)media_tx_enqueue(&event);
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
