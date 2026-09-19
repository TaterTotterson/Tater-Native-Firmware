#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TATER_BLE_ADV_DATA_MAX 31
#define TATER_BLE_BATCH_MAX 12

typedef struct {
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi;
    uint8_t event_type;
    uint8_t data_len;
    uint8_t data[TATER_BLE_ADV_DATA_MAX];
    uint32_t observed_ms;
} tater_ble_advert_t;

typedef struct {
    bool supported;
    bool enabled;
    bool initialized;
    bool synced;
    bool scanning;
    bool paused_for_audio;
    uint32_t adverts_seen;
    uint32_t adverts_sent;
    uint32_t adverts_filtered;
    uint32_t adverts_dropped;
    uint32_t batches_sent;
    uint32_t controller_resets;
    int last_error;
    uint32_t internal_heap_free;
    uint32_t internal_heap_min;
} tater_ble_scanner_stats_t;

typedef bool (*tater_ble_batch_callback_t)(
    const tater_ble_advert_t *adverts,
    size_t count,
    uint32_t batch_id
);

bool tater_ble_scanner_supported(void);
void tater_ble_scanner_init(tater_ble_batch_callback_t batch_callback);
void tater_ble_scanner_poll(bool audio_busy);
void tater_ble_scanner_get_stats(tater_ble_scanner_stats_t *stats);
