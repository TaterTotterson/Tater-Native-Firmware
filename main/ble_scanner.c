#include "ble_scanner.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

void ble_store_config_init(void);
#endif

static const char *TAG = "tater_ble";

#define TATER_BLE_DEDUPE_ENTRIES 48
#define TATER_BLE_RSSI_DELTA_DB 3
#define TATER_BLE_EMIT_MIN_MS 250
#define TATER_BLE_EMIT_MAX_MS 1000
#define TATER_BLE_BATCH_FLUSH_MS 500
#define TATER_BLE_SCAN_INTERVAL_UNITS 512 /* 320 ms in 0.625 ms units. */
#define TATER_BLE_SCAN_WINDOW_UNITS 48    /* 30 ms in 0.625 ms units. */
#define TATER_BLE_ENROLLMENT_EVENTS 4
#define TATER_BLE_ENROLLMENT_DEFAULT_TIMEOUT_S 90
#define TATER_BLE_ENROLLMENT_MIN_TIMEOUT_S 30
#define TATER_BLE_ENROLLMENT_MAX_TIMEOUT_S 180

typedef struct {
    bool used;
    uint8_t address[6];
    uint8_t address_type;
    uint8_t event_type;
    uint32_t payload_hash;
    int8_t last_rssi;
    uint32_t last_emit_ms;
    uint32_t last_seen_ms;
} tater_ble_dedupe_entry_t;

static portMUX_TYPE s_ble_lock = portMUX_INITIALIZER_UNLOCKED;
static tater_ble_batch_callback_t s_batch_callback;
static volatile bool s_requested_enabled;
static volatile bool s_initialized;
static volatile bool s_synced;
static volatile bool s_scanning;
static volatile bool s_paused_for_audio;
static tater_ble_advert_t s_batch[TATER_BLE_BATCH_MAX];
static size_t s_batch_count;
static uint32_t s_batch_id;
static uint32_t s_last_flush_ms;
static tater_ble_dedupe_entry_t s_dedupe[TATER_BLE_DEDUPE_ENTRIES];
static tater_ble_scanner_stats_t s_stats;
static tater_ble_enrollment_event_t s_enrollment_events[TATER_BLE_ENROLLMENT_EVENTS];
static size_t s_enrollment_event_head;
static size_t s_enrollment_event_count;

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
typedef struct {
    bool active;
    bool capture_pending;
    bool peer_known;
    bool announced_advertising;
    char enrollment_id[TATER_BLE_ENROLLMENT_ID_MAX];
    char display_name[TATER_BLE_ENROLLMENT_NAME_MAX];
    uint32_t deadline_ms;
    uint16_t conn_handle;
    ble_addr_t peer_id_addr;
} tater_ble_enrollment_state_t;

static tater_ble_enrollment_state_t s_enrollment = {
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};
static const ble_uuid16_t s_heart_rate_uuid = BLE_UUID16_INIT(0x180d);

static int enrollment_gatt_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg
)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    static const uint8_t body_sensor_location = 0x00;
    if (ble_uuid_u16(ctxt->chr->uuid) == 0x2a38) {
        return os_mbuf_append(ctxt->om, &body_sensor_location, sizeof(body_sensor_location)) == 0
            ? 0
            : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_enrollment_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180d),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2a37),
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2a38),
                .access_cb = enrollment_gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {0},
        },
    },
    {0},
};

static int enrollment_gatt_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_enrollment_services);
    return rc == 0 ? ble_gatts_add_svcs(s_enrollment_services) : rc;
}
#endif

static void queue_enrollment_event(
    tater_ble_enrollment_event_type_t type,
    const char *enrollment_id,
    const char *status,
    bool ok,
    const uint8_t *irk,
    const char *error
)
{
    portENTER_CRITICAL(&s_ble_lock);
    if (s_enrollment_event_count == TATER_BLE_ENROLLMENT_EVENTS) {
        memset(&s_enrollment_events[s_enrollment_event_head], 0, sizeof(s_enrollment_events[0]));
        s_enrollment_event_head = (s_enrollment_event_head + 1) % TATER_BLE_ENROLLMENT_EVENTS;
        s_enrollment_event_count--;
    }
    size_t index = (s_enrollment_event_head + s_enrollment_event_count)
        % TATER_BLE_ENROLLMENT_EVENTS;
    tater_ble_enrollment_event_t *event = &s_enrollment_events[index];
    memset(event, 0, sizeof(*event));
    event->type = type;
    event->ok = ok;
    snprintf(event->enrollment_id, sizeof(event->enrollment_id), "%s", enrollment_id ? enrollment_id : "");
    snprintf(event->status, sizeof(event->status), "%s", status ? status : "");
    snprintf(event->error, sizeof(event->error), "%s", error ? error : "");
    if (irk) {
        memcpy(event->irk, irk, sizeof(event->irk));
    }
    s_enrollment_event_count++;
    portEXIT_CRITICAL(&s_ble_lock);
}

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t fnv1a_update(uint32_t hash, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t advert_hash(
    const uint8_t address[6],
    uint8_t address_type,
    uint8_t event_type,
    const uint8_t *data,
    size_t data_len
)
{
    uint32_t hash = 2166136261u;
    hash = fnv1a_update(hash, address, 6);
    hash = fnv1a_update(hash, &address_type, 1);
    hash = fnv1a_update(hash, &event_type, 1);
    return fnv1a_update(hash, data, data_len);
}

static bool elapsed_at_least(uint32_t now_ms, uint32_t then_ms, uint32_t duration_ms)
{
    return (uint32_t)(now_ms - then_ms) >= duration_ms;
}

static bool advert_should_emit(
    const uint8_t address[6],
    uint8_t address_type,
    uint8_t event_type,
    const uint8_t *data,
    size_t data_len,
    int8_t rssi,
    uint32_t now_ms
)
{
    const uint32_t hash = advert_hash(address, address_type, event_type, data, data_len);
    tater_ble_dedupe_entry_t *entry = NULL;
    tater_ble_dedupe_entry_t *oldest = &s_dedupe[0];

    for (size_t i = 0; i < TATER_BLE_DEDUPE_ENTRIES; i++) {
        tater_ble_dedupe_entry_t *candidate = &s_dedupe[i];
        if (!candidate->used) {
            entry = candidate;
            break;
        }
        if (
            candidate->payload_hash == hash
            && candidate->address_type == address_type
            && candidate->event_type == event_type
            && memcmp(candidate->address, address, 6) == 0
        ) {
            entry = candidate;
            break;
        }
        if ((int32_t)(candidate->last_seen_ms - oldest->last_seen_ms) < 0) {
            oldest = candidate;
        }
    }

    if (!entry) {
        entry = oldest;
    }

    if (!entry->used || entry->payload_hash != hash
            || entry->address_type != address_type
            || entry->event_type != event_type
            || memcmp(entry->address, address, 6) != 0) {
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        memcpy(entry->address, address, 6);
        entry->address_type = address_type;
        entry->event_type = event_type;
        entry->payload_hash = hash;
        entry->last_rssi = rssi;
        entry->last_emit_ms = now_ms;
        entry->last_seen_ms = now_ms;
        return true;
    }

    bool emit = elapsed_at_least(now_ms, entry->last_emit_ms, TATER_BLE_EMIT_MAX_MS);
    int rssi_delta = abs((int)rssi - (int)entry->last_rssi);
    if (!emit && rssi_delta >= TATER_BLE_RSSI_DELTA_DB) {
        emit = elapsed_at_least(now_ms, entry->last_emit_ms, TATER_BLE_EMIT_MIN_MS);
    }

    entry->last_seen_ms = now_ms;
    if (emit) {
        entry->last_emit_ms = now_ms;
        entry->last_rssi = rssi;
    }
    return emit;
}

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
static void start_scan(void);
static int gap_event(struct ble_gap_event *event, void *arg);

static bool enrollment_active(void)
{
    portENTER_CRITICAL(&s_ble_lock);
    bool active = s_enrollment.active;
    portEXIT_CRITICAL(&s_ble_lock);
    return active;
}

static int start_enrollment_advertising(void)
{
    char display_name[TATER_BLE_ENROLLMENT_NAME_MAX];
    portENTER_CRITICAL(&s_ble_lock);
    bool active = s_enrollment.active;
    snprintf(display_name, sizeof(display_name), "%s", s_enrollment.display_name);
    portEXIT_CRITICAL(&s_ble_lock);
    if (!active || !s_synced) {
        return BLE_HS_EINVAL;
    }

    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        return rc;
    }
    rc = ble_svc_gap_device_name_set(display_name);
    if (rc != 0) {
        return rc;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)display_name;
    fields.name_len = strlen(display_name);
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t *)&s_heart_rate_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        return rc;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}

static bool capture_enrollment_irk(void)
{
    uint16_t conn_handle;
    char enrollment_id[TATER_BLE_ENROLLMENT_ID_MAX];
    portENTER_CRITICAL(&s_ble_lock);
    bool pending = s_enrollment.active && s_enrollment.capture_pending;
    conn_handle = s_enrollment.conn_handle;
    snprintf(enrollment_id, sizeof(enrollment_id), "%s", s_enrollment.enrollment_id);
    portEXIT_CRITICAL(&s_ble_lock);
    if (!pending || conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }

    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        return false;
    }
    struct ble_store_key_sec key = {0};
    key.peer_addr = desc.peer_id_addr;
    struct ble_store_value_sec value = {0};
    rc = ble_store_read_peer_sec(&key, &value);
    if (rc != 0 || !value.irk_present) {
        return false;
    }

    uint8_t irk[16];
    memcpy(irk, value.irk, sizeof(irk));
    ble_addr_t peer_id_addr = value.peer_addr;
    portENTER_CRITICAL(&s_ble_lock);
    s_enrollment.active = false;
    s_enrollment.capture_pending = false;
    s_enrollment.peer_known = true;
    s_enrollment.peer_id_addr = peer_id_addr;
    portEXIT_CRITICAL(&s_ble_lock);
    queue_enrollment_event(
        TATER_BLE_ENROLLMENT_EVENT_RESULT,
        enrollment_id,
        "complete",
        true,
        irk,
        NULL
    );
    memset(irk, 0, sizeof(irk));
    (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    (void)ble_store_util_delete_peer(&peer_id_addr);
    ESP_LOGI(TAG, "BLE enrollment complete; temporary bond removed");
    return true;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (!event) {
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_CONNECT && enrollment_active()) {
        if (event->connect.status != 0) {
            int rc = start_enrollment_advertising();
            if (rc != 0) {
                ESP_LOGW(TAG, "BLE enrollment advertising restart failed rc=%d", rc);
            }
            return 0;
        }
        char enrollment_id[TATER_BLE_ENROLLMENT_ID_MAX];
        portENTER_CRITICAL(&s_ble_lock);
        s_enrollment.conn_handle = event->connect.conn_handle;
        snprintf(enrollment_id, sizeof(enrollment_id), "%s", s_enrollment.enrollment_id);
        portEXIT_CRITICAL(&s_ble_lock);
        queue_enrollment_event(
            TATER_BLE_ENROLLMENT_EVENT_STATUS,
            enrollment_id,
            "pairing",
            true,
            NULL,
            NULL
        );
        int rc = ble_gap_security_initiate(event->connect.conn_handle);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "BLE enrollment security initiation failed rc=%d", rc);
        }
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_ENC_CHANGE && enrollment_active()) {
        if (event->enc_change.status == 0) {
            portENTER_CRITICAL(&s_ble_lock);
            s_enrollment.capture_pending = true;
            portEXIT_CRITICAL(&s_ble_lock);
            (void)capture_enrollment_irk();
        }
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        bool active = enrollment_active();
        portENTER_CRITICAL(&s_ble_lock);
        if (s_enrollment.conn_handle == event->disconnect.conn.conn_handle) {
            s_enrollment.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
        portEXIT_CRITICAL(&s_ble_lock);
        if (active) {
            int rc = start_enrollment_advertising();
            if (rc != 0) {
                ESP_LOGW(TAG, "BLE enrollment advertising resume failed rc=%d", rc);
            }
        }
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE && enrollment_active()) {
        (void)start_enrollment_advertising();
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        portENTER_CRITICAL(&s_ble_lock);
        s_scanning = false;
        portEXIT_CRITICAL(&s_ble_lock);
        return 0;
    }
    if (
        event->type != BLE_GAP_EVENT_DISC
        || !s_requested_enabled
        || s_paused_for_audio
        || enrollment_active()
    ) {
        return 0;
    }

    const struct ble_gap_disc_desc *disc = &event->disc;
    uint32_t now_ms = monotonic_ms();
    size_t data_len = disc->length_data;
    if (!disc->data) {
        data_len = 0;
    }
    if (data_len > TATER_BLE_ADV_DATA_MAX) {
        data_len = TATER_BLE_ADV_DATA_MAX;
    }

    portENTER_CRITICAL(&s_ble_lock);
    s_stats.adverts_seen++;
    bool emit = advert_should_emit(
        disc->addr.val,
        disc->addr.type,
        disc->event_type,
        disc->data,
        data_len,
        disc->rssi,
        now_ms
    );
    if (!emit) {
        s_stats.adverts_filtered++;
        portEXIT_CRITICAL(&s_ble_lock);
        return 0;
    }
    if (s_batch_count >= TATER_BLE_BATCH_MAX) {
        s_stats.adverts_dropped++;
        portEXIT_CRITICAL(&s_ble_lock);
        return 0;
    }

    tater_ble_advert_t *advert = &s_batch[s_batch_count++];
    memset(advert, 0, sizeof(*advert));
    memcpy(advert->address, disc->addr.val, sizeof(advert->address));
    advert->address_type = disc->addr.type;
    advert->rssi = disc->rssi;
    advert->event_type = disc->event_type;
    advert->data_len = (uint8_t)data_len;
    if (data_len > 0 && disc->data) {
        memcpy(advert->data, disc->data, data_len);
    }
    advert->observed_ms = now_ms;
    portEXIT_CRITICAL(&s_ble_lock);
    return 0;
}

static void on_reset(int reason)
{
    portENTER_CRITICAL(&s_ble_lock);
    s_synced = false;
    s_scanning = false;
    s_stats.controller_resets++;
    s_stats.last_error = reason;
    bool enrollment_was_active = s_enrollment.active;
    char enrollment_id[TATER_BLE_ENROLLMENT_ID_MAX];
    snprintf(enrollment_id, sizeof(enrollment_id), "%s", s_enrollment.enrollment_id);
    memset(&s_enrollment, 0, sizeof(s_enrollment));
    s_enrollment.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    portEXIT_CRITICAL(&s_ble_lock);
    if (enrollment_was_active) {
        queue_enrollment_event(
            TATER_BLE_ENROLLMENT_EVENT_RESULT,
            enrollment_id,
            "failed",
            false,
            NULL,
            "Bluetooth controller reset during enrollment."
        );
    }
    ESP_LOGW(TAG, "NimBLE host reset reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    portENTER_CRITICAL(&s_ble_lock);
    s_synced = rc == 0;
    s_stats.last_error = rc;
    portEXIT_CRITICAL(&s_ble_lock);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE identity address unavailable rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "NimBLE observer and enrollment peripheral ready");
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void start_stack(void)
{
    if (s_initialized) {
        return;
    }
    uint32_t before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_ble_lock);
        /*
         * Controller initialization needs internal DMA-capable memory even
         * when NimBLE host allocations live in PSRAM.  Some satellites run
         * with very little internal heap after audio and Wi-Fi start.  Do not
         * retry every poll in that condition: repeated controller setup can
         * starve the WebSocket task and disconnect the satellite.
         * A reboot clears this latch and lets the always-on scanner retry from
         * a clean controller state.
         */
        s_requested_enabled = false;
        s_stats.last_error = err;
        portEXIT_CRITICAL(&s_ble_lock);
        ESP_LOGE(TAG, "NimBLE init failed: %s; scanner disabled until reboot", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    int rc = enrollment_gatt_init();
    if (rc != 0) {
        portENTER_CRITICAL(&s_ble_lock);
        s_requested_enabled = false;
        s_stats.last_error = rc;
        portEXIT_CRITICAL(&s_ble_lock);
        ESP_LOGE(TAG, "BLE enrollment GATT init failed rc=%d", rc);
        return;
    }
    ble_store_config_init();
    s_initialized = true;
    nimble_port_freertos_init(host_task);
    uint32_t after = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "NimBLE started internal_heap_before=%u after=%u delta=%d",
        (unsigned)before,
        (unsigned)after,
        (int)before - (int)after);
}

static void start_scan(void)
{
    if (
        !s_synced
        || !s_requested_enabled
        || s_paused_for_audio
        || enrollment_active()
        || ble_gap_disc_active()
    ) {
        return;
    }
    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        portENTER_CRITICAL(&s_ble_lock);
        s_stats.last_error = rc;
        portEXIT_CRITICAL(&s_ble_lock);
        return;
    }

    struct ble_gap_disc_params params = {0};
    params.passive = 1;
    params.filter_duplicates = 0;
    params.itvl = TATER_BLE_SCAN_INTERVAL_UNITS;
    params.window = TATER_BLE_SCAN_WINDOW_UNITS;
    params.filter_policy = 0;
    params.limited = 0;
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    portENTER_CRITICAL(&s_ble_lock);
    s_scanning = rc == 0;
    s_stats.last_error = rc;
    portEXIT_CRITICAL(&s_ble_lock);
    if (rc != 0) {
        ESP_LOGW(TAG, "BLE passive scan start failed rc=%d", rc);
    }
}

static void stop_scan(void)
{
    if (!s_initialized || !ble_gap_disc_active()) {
        portENTER_CRITICAL(&s_ble_lock);
        s_scanning = false;
        portEXIT_CRITICAL(&s_ble_lock);
        return;
    }
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        portENTER_CRITICAL(&s_ble_lock);
        s_stats.last_error = rc;
        portEXIT_CRITICAL(&s_ble_lock);
        ESP_LOGW(TAG, "BLE passive scan stop failed rc=%d", rc);
    }
}
#endif

bool tater_ble_scanner_supported(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    return true;
#else
    return false;
#endif
}

bool tater_ble_enrollment_supported(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    return true;
#else
    return false;
#endif
}

esp_err_t tater_ble_enrollment_start(
    const char *enrollment_id,
    const char *display_name,
    uint32_t timeout_s
)
{
    if (!tater_ble_enrollment_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!enrollment_id || !enrollment_id[0] || !display_name || !display_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    portENTER_CRITICAL(&s_ble_lock);
    if (!s_initialized || !s_synced || s_enrollment.active) {
        portEXIT_CRITICAL(&s_ble_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_s < TATER_BLE_ENROLLMENT_MIN_TIMEOUT_S) {
        timeout_s = TATER_BLE_ENROLLMENT_MIN_TIMEOUT_S;
    }
    if (timeout_s > TATER_BLE_ENROLLMENT_MAX_TIMEOUT_S) {
        timeout_s = TATER_BLE_ENROLLMENT_MAX_TIMEOUT_S;
    }
    memset(&s_enrollment, 0, sizeof(s_enrollment));
    s_enrollment.active = true;
    s_enrollment.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_enrollment.deadline_ms = monotonic_ms() + (timeout_s * 1000U);
    snprintf(s_enrollment.enrollment_id, sizeof(s_enrollment.enrollment_id), "%s", enrollment_id);
    snprintf(s_enrollment.display_name, sizeof(s_enrollment.display_name), "%s", display_name);
    portEXIT_CRITICAL(&s_ble_lock);

    stop_scan();
    queue_enrollment_event(
        TATER_BLE_ENROLLMENT_EVENT_STATUS,
        enrollment_id,
        "starting",
        true,
        NULL,
        NULL
    );
    ESP_LOGI(TAG, "BLE enrollment window opened for %u seconds", (unsigned)timeout_s);
    return ESP_OK;
#else
    (void)timeout_s;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void tater_ble_enrollment_cancel(const char *enrollment_id)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    char active_id[TATER_BLE_ENROLLMENT_ID_MAX];
    uint16_t conn_handle;
    ble_addr_t peer = {0};
    portENTER_CRITICAL(&s_ble_lock);
    bool active = s_enrollment.active;
    snprintf(active_id, sizeof(active_id), "%s", s_enrollment.enrollment_id);
    bool matches = !enrollment_id || !enrollment_id[0] || strcmp(enrollment_id, active_id) == 0;
    conn_handle = s_enrollment.conn_handle;
    bool peer_known = s_enrollment.peer_known;
    peer = s_enrollment.peer_id_addr;
    if (active && matches) {
        memset(&s_enrollment, 0, sizeof(s_enrollment));
        s_enrollment.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    portEXIT_CRITICAL(&s_ble_lock);
    if (!active || !matches) {
        return;
    }
    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (peer_known) {
        (void)ble_store_util_delete_peer(&peer);
    }
    queue_enrollment_event(
        TATER_BLE_ENROLLMENT_EVENT_STATUS,
        active_id,
        "cancelled",
        true,
        NULL,
        NULL
    );
    ESP_LOGI(TAG, "BLE enrollment cancelled");
#else
    (void)enrollment_id;
#endif
}

bool tater_ble_enrollment_take_event(tater_ble_enrollment_event_t *event)
{
    if (!event) {
        return false;
    }
    portENTER_CRITICAL(&s_ble_lock);
    if (s_enrollment_event_count == 0) {
        portEXIT_CRITICAL(&s_ble_lock);
        memset(event, 0, sizeof(*event));
        return false;
    }
    *event = s_enrollment_events[s_enrollment_event_head];
    memset(&s_enrollment_events[s_enrollment_event_head], 0, sizeof(s_enrollment_events[0]));
    s_enrollment_event_head = (s_enrollment_event_head + 1) % TATER_BLE_ENROLLMENT_EVENTS;
    s_enrollment_event_count--;
    portEXIT_CRITICAL(&s_ble_lock);
    return true;
}

void tater_ble_scanner_init(tater_ble_batch_callback_t batch_callback)
{
    bool supported = tater_ble_scanner_supported();
    portENTER_CRITICAL(&s_ble_lock);
    s_batch_callback = batch_callback;
    s_last_flush_ms = monotonic_ms();
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.supported = supported;
    s_requested_enabled = supported;
    portEXIT_CRITICAL(&s_ble_lock);
}

void tater_ble_scanner_poll(bool audio_busy)
{
    uint32_t now_ms = monotonic_ms();
    bool enrollment_running = false;
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    enrollment_running = enrollment_active();
#endif
    s_paused_for_audio = s_requested_enabled && audio_busy && !enrollment_running;

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    if (s_requested_enabled && !s_initialized) {
        start_stack();
    }
    bool active = enrollment_active();
    if (active) {
        uint32_t deadline_ms;
        uint16_t conn_handle;
        char enrollment_id[TATER_BLE_ENROLLMENT_ID_MAX];
        portENTER_CRITICAL(&s_ble_lock);
        deadline_ms = s_enrollment.deadline_ms;
        conn_handle = s_enrollment.conn_handle;
        snprintf(enrollment_id, sizeof(enrollment_id), "%s", s_enrollment.enrollment_id);
        portEXIT_CRITICAL(&s_ble_lock);
        if ((int32_t)(now_ms - deadline_ms) >= 0) {
            ble_addr_t peer = {0};
            bool peer_known;
            portENTER_CRITICAL(&s_ble_lock);
            peer_known = s_enrollment.peer_known;
            peer = s_enrollment.peer_id_addr;
            memset(&s_enrollment, 0, sizeof(s_enrollment));
            s_enrollment.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            portEXIT_CRITICAL(&s_ble_lock);
            if (ble_gap_adv_active()) {
                (void)ble_gap_adv_stop();
            }
            if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            if (peer_known) {
                (void)ble_store_util_delete_peer(&peer);
            }
            queue_enrollment_event(
                TATER_BLE_ENROLLMENT_EVENT_STATUS,
                enrollment_id,
                "timed_out",
                false,
                NULL,
                "No device paired before the enrollment window closed."
            );
            active = false;
        } else {
            (void)capture_enrollment_irk();
            active = enrollment_active();
            if (
                active
                && conn_handle == BLE_HS_CONN_HANDLE_NONE
                && !ble_gap_adv_active()
                && !ble_gap_disc_active()
            ) {
                int rc = start_enrollment_advertising();
                if (rc == 0) {
                    bool announce = false;
                    portENTER_CRITICAL(&s_ble_lock);
                    if (!s_enrollment.announced_advertising) {
                        s_enrollment.announced_advertising = true;
                        announce = true;
                    }
                    portEXIT_CRITICAL(&s_ble_lock);
                    if (announce) {
                        queue_enrollment_event(
                            TATER_BLE_ENROLLMENT_EVENT_STATUS,
                            enrollment_id,
                            "advertising",
                            true,
                            NULL,
                            NULL
                        );
                    }
                } else if (rc != BLE_HS_EALREADY) {
                    portENTER_CRITICAL(&s_ble_lock);
                    s_stats.last_error = rc;
                    portEXIT_CRITICAL(&s_ble_lock);
                }
            }
        }
    }
    bool want_scan = s_requested_enabled && s_synced && !audio_busy && !active;
    if (want_scan) {
        start_scan();
    } else if (s_scanning || (s_initialized && ble_gap_disc_active())) {
        stop_scan();
    }
#endif

    if (audio_busy || !elapsed_at_least(now_ms, s_last_flush_ms, TATER_BLE_BATCH_FLUSH_MS)) {
        return;
    }
    s_last_flush_ms = now_ms;

    tater_ble_advert_t local_batch[TATER_BLE_BATCH_MAX];
    size_t local_count = 0;
    uint32_t batch_id = 0;
    tater_ble_batch_callback_t callback = NULL;
    portENTER_CRITICAL(&s_ble_lock);
    if (s_batch_count > 0) {
        local_count = s_batch_count;
        memcpy(local_batch, s_batch, local_count * sizeof(local_batch[0]));
        s_batch_count = 0;
        batch_id = ++s_batch_id;
        callback = s_batch_callback;
    }
    portEXIT_CRITICAL(&s_ble_lock);

    if (local_count == 0) {
        return;
    }
    bool sent = callback && callback(local_batch, local_count, batch_id);
    portENTER_CRITICAL(&s_ble_lock);
    if (sent) {
        s_stats.adverts_sent += (uint32_t)local_count;
        s_stats.batches_sent++;
    } else {
        s_stats.adverts_dropped += (uint32_t)local_count;
    }
    portEXIT_CRITICAL(&s_ble_lock);
}

void tater_ble_scanner_get_stats(tater_ble_scanner_stats_t *stats)
{
    if (!stats) {
        return;
    }
    portENTER_CRITICAL(&s_ble_lock);
    *stats = s_stats;
    stats->supported = tater_ble_scanner_supported();
    stats->enabled = s_requested_enabled;
    stats->initialized = s_initialized;
    stats->synced = s_synced;
    stats->scanning = s_scanning;
    stats->paused_for_audio = s_paused_for_audio;
    portEXIT_CRITICAL(&s_ble_lock);
    stats->internal_heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    stats->internal_heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
