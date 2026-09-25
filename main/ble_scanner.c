#include "ble_scanner.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#endif

static const char *TAG = "tater_ble";

#define TATER_BLE_DEDUPE_ENTRIES 48
#define TATER_BLE_RSSI_DELTA_DB 3
#define TATER_BLE_EMIT_MIN_MS 250
#define TATER_BLE_EMIT_MAX_MS 1000
#define TATER_BLE_BATCH_FLUSH_MS 500
#define TATER_BLE_SCAN_INTERVAL_UNITS 512 /* 320 ms in 0.625 ms units. */
#define TATER_BLE_SCAN_WINDOW_UNITS 48    /* 30 ms in 0.625 ms units. */

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

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (!event) {
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        portENTER_CRITICAL(&s_ble_lock);
        s_scanning = false;
        portEXIT_CRITICAL(&s_ble_lock);
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC || !s_requested_enabled || s_paused_for_audio) {
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
    portEXIT_CRITICAL(&s_ble_lock);
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
    ESP_LOGI(TAG, "NimBLE observer ready");
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
    if (!s_synced || !s_requested_enabled || s_paused_for_audio || ble_gap_disc_active()) {
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
    s_paused_for_audio = s_requested_enabled && audio_busy;

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    if (s_requested_enabled && !s_initialized) {
        start_stack();
    }
    bool want_scan = s_requested_enabled && s_synced && !audio_busy;
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
