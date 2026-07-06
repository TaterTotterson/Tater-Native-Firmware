#include "tater_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "board.h"
#include "cJSON.h"
#include "esp_core_dump.h"
#include "esp_err.h"
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
#include "audio_i2s.h"
#include "native_settings.h"
#include "playback.h"
#include "wake_engine.h"

static const char *TAG = "tater_proto";
static const char *NATIVE_WS_PATH = "/api/tater/satellite/v1/ws";

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
static int64_t s_last_link_down_us;
static int64_t s_last_reconnect_attempt_us;
static int64_t s_last_hello_us;
static bool s_hello_acked;
static bool s_playback_return_armed;
static tater_state_t s_playback_return_state = TATER_STATE_IDLE;
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
static void timer_monitor_task(void *arg);

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

#define TATER_WS_RECONNECT_AFTER_MS 7000
#define TATER_WS_RECONNECT_MIN_INTERVAL_MS 7000
#define TATER_WS_HELLO_ACK_TIMEOUT_MS 5000

#define TATER_AUDIO_PREROLL_SAMPLES (TATER_MIC_SAMPLE_RATE)
static int16_t s_audio_preroll[TATER_AUDIO_PREROLL_SAMPLES];
static size_t s_audio_preroll_start;
static size_t s_audio_preroll_count;

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
        && tater_playback_is_playing()) {
        bool playback_finished = detail
            && (strcmp(detail, "playback return") == 0
                || strcmp(detail, "playback finished") == 0
                || strcmp(detail, "playback stopped") == 0);
        if (!playback_finished) {
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
    s_audio_preroll_start = 0;
    s_audio_preroll_count = 0;
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
        clear_audio_preroll_locked();
        xSemaphoreGive(s_send_lock);
    } else {
        s_voice_active = false;
        s_voice_start_pending = false;
        s_audio_preroll_start = 0;
        s_audio_preroll_count = 0;
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

    int16_t chunk[TATER_MIC_CHUNK_FRAMES];
    size_t flushed = 0;
    while (s_audio_preroll_count > 0) {
        size_t chunk_samples = s_audio_preroll_count < TATER_MIC_CHUNK_FRAMES ? s_audio_preroll_count : TATER_MIC_CHUNK_FRAMES;
        for (size_t i = 0; i < chunk_samples; i++) {
            chunk[i] = s_audio_preroll[(s_audio_preroll_start + i) % TATER_AUDIO_PREROLL_SAMPLES];
        }
        int sent = send_audio_locked(chunk, chunk_samples, pdMS_TO_TICKS(1000));
        if (sent < 0) {
            ESP_LOGW(TAG, "audio preroll flush failed samples=%u result=%d", (unsigned)chunk_samples, sent);
            clear_audio_preroll_locked();
            mark_link_down("websocket audio preroll send failed");
            return;
        }
        s_audio_preroll_start = (s_audio_preroll_start + chunk_samples) % TATER_AUDIO_PREROLL_SAMPLES;
        s_audio_preroll_count -= chunk_samples;
        flushed += chunk_samples;
    }
    if (flushed > 0) {
        ESP_LOGI(TAG, "audio preroll flushed samples=%u", (unsigned)flushed);
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
    if (s_current_state == TATER_STATE_TIMER) {
        emit_state(websocket_ready() ? TATER_STATE_IDLE : TATER_STATE_DISCONNECTED, "timer armed");
    }
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
    cJSON_AddBoolToObject(caps, "display", false);
    cJSON_AddBoolToObject(caps, "buttons", true);
    cJSON_AddBoolToObject(caps, "touch", false);
    cJSON_AddBoolToObject(caps, "line_out", false);
    cJSON_AddBoolToObject(caps, "local_wake", true);
    cJSON_AddBoolToObject(caps, "live_settings", true);
    cJSON_AddBoolToObject(caps, "setup_mode", true);
    cJSON_AddBoolToObject(caps, "continued_chat_reopen", true);
    cJSON_AddBoolToObject(caps, "barge_in", true);
    cJSON_AddBoolToObject(caps, "tool_call_mode", true);
    cJSON_AddBoolToObject(caps, "timers", true);
    cJSON_AddBoolToObject(caps, "ota", true);
    cJSON_AddBoolToObject(caps, "xmos", true);
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
            if (s_pending_reopen && !tater_playback_is_playing()) {
                ESP_LOGI(TAG, "continued chat reopen cleared; run ended without active playback");
                s_pending_reopen = false;
                s_pending_reopen_conversation_id[0] = '\0';
            }
        } else if (strcmp(event, "ERROR") == 0) {
            clear_voice_capture_state();
            s_pending_reopen = false;
            s_pending_reopen_conversation_id[0] = '\0';
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
    if (s_current_state == TATER_STATE_IDLE) {
        return true;
    }
    const tater_live_settings_t *settings = tater_live_settings_get();
    return settings && settings->barge_in_enabled && tater_playback_is_playing();
}

const char *tater_protocol_device_id(void)
{
    return s_device_id;
}

const char *tater_protocol_device_name(void)
{
    return s_config.device_name[0] ? s_config.device_name : s_device_id;
}

const char *tater_protocol_server_url(void)
{
    return s_config.server_url;
}

void tater_protocol_send_status(const char *state)
{
    cJSON *root = new_envelope("status");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "state", state ? state : "idle");
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
    cJSON_AddNumberToObject(transport, "last_ws_error_type", s_last_ws_error_type);
    cJSON_AddNumberToObject(transport, "last_ws_tls_err", s_last_ws_tls_err);
    cJSON_AddNumberToObject(transport, "last_ws_stack_err", s_last_ws_stack_err);
    cJSON_AddNumberToObject(transport, "last_ws_sock_errno", s_last_ws_sock_errno);
    cJSON_AddNumberToObject(transport, "last_ws_http_status", s_last_ws_http_status);
    cJSON_AddItemToObject(payload, "transport", transport);
    cJSON_AddItemToObject(payload, "reset", reset_diag_json());
    tater_live_settings_add_status(payload);
    tater_wake_engine_add_status(payload);
    tater_audio_doa_t doa = {0};
    if (tater_audio_doa_snapshot(&doa)) {
        cJSON *xmos = cJSON_CreateObject();
        cJSON_AddBoolToObject(xmos, "valid", doa.valid);
        cJSON_AddNumberToObject(xmos, "confidence", doa.confidence);
        cJSON_AddNumberToObject(xmos, "sample_delay", doa.sample_delay);
        cJSON_AddNumberToObject(xmos, "energy", doa.energy);
        cJSON_AddNumberToObject(xmos, "frame_counter", doa.frame_counter);
        cJSON_AddNumberToObject(xmos, "age_ms", doa.age_ms);
        cJSON_AddItemToObject(payload, "xmos_doa", xmos);
    }
    tater_audio_xmos_status_t xmos_status = {0};
    if (tater_audio_xmos_status_snapshot(&xmos_status)) {
        char installed[16] = {0};
        char target[16] = {0};
        snprintf(
            target,
            sizeof(target),
            "%u.%u.%u",
            xmos_status.target_major,
            xmos_status.target_minor,
            xmos_status.target_patch
        );
        if (xmos_status.version_valid) {
            snprintf(
                installed,
                sizeof(installed),
                "%u.%u.%u",
                xmos_status.major,
                xmos_status.minor,
                xmos_status.patch
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
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    clear_audio_preroll_locked();
    s_voice_active = true;
    s_voice_start_pending = true;
    s_audio_send_logs = 0;
    s_audio_send_failures = 0;
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
}

void tater_protocol_start_voice(const char *wake_word, const char *source)
{
    tater_protocol_start_voice_with_conversation(wake_word, source, "");
}

void tater_protocol_stop_voice(bool abort)
{
    if (!websocket_ready()) {
        clear_voice_capture_state();
        return;
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
    int expected = (int)(sample_count * sizeof(int16_t));
    int sent = send_audio_locked(pcm, sample_count, pdMS_TO_TICKS(1500));
    xSemaphoreGive(s_send_lock);
    bool failed = sent < expected;
    s_last_audio_send_result = sent;
    s_last_audio_send_samples = (uint32_t)sample_count;
    if (s_audio_send_logs < 3 || failed) {
        ESP_LOGI(TAG, "audio bin send samples=%u bytes=%u result=%d", (unsigned)sample_count, (unsigned)(sample_count * sizeof(int16_t)), sent);
        s_audio_send_logs++;
    }
    if (failed) {
        s_audio_send_failures++;
        s_audio_send_failure_total++;
        if (s_audio_send_failures >= 8) {
            mark_link_down("websocket audio send failed");
        }
    } else {
        s_audio_send_failures = 0;
    }
}

void tater_protocol_send_log(const char *level, const char *message)
{
    cJSON *root = new_envelope("log");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON_AddStringToObject(payload, "level", level ? level : "info");
    cJSON_AddStringToObject(payload, "message", message ? message : "");
    send_json(root);
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
    }

    if (should_reopen && conversation_id[0]) {
        tater_reopen_args_t *request = (tater_reopen_args_t *)calloc(1, sizeof(tater_reopen_args_t));
        if (request) {
            snprintf(request->conversation_id, sizeof(request->conversation_id), "%s", conversation_id);
            if (xTaskCreate(continued_reopen_task, "tater_reopen", 4096, request, 5, NULL) != pdPASS) {
                free(request);
                ESP_LOGW(TAG, "continued chat reopen task create failed");
            }
        } else {
            ESP_LOGW(TAG, "continued chat reopen alloc failed");
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
