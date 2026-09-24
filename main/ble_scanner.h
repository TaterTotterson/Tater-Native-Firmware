#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TATER_BLE_ADV_DATA_MAX 31
#define TATER_BLE_BATCH_MAX 12
#define TATER_BLE_ENROLLMENT_ID_MAX 40
#define TATER_BLE_ENROLLMENT_NAME_MAX 32

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

typedef enum {
    TATER_BLE_ENROLLMENT_EVENT_NONE = 0,
    TATER_BLE_ENROLLMENT_EVENT_STATUS,
    TATER_BLE_ENROLLMENT_EVENT_RESULT,
} tater_ble_enrollment_event_type_t;

typedef struct {
    tater_ble_enrollment_event_type_t type;
    char enrollment_id[TATER_BLE_ENROLLMENT_ID_MAX];
    char status[20];
    char error[96];
    bool ok;
    uint8_t irk[16];
} tater_ble_enrollment_event_t;

bool tater_ble_scanner_supported(void);
void tater_ble_scanner_init(tater_ble_batch_callback_t batch_callback);
void tater_ble_scanner_poll(bool audio_busy);
void tater_ble_scanner_get_stats(tater_ble_scanner_stats_t *stats);
bool tater_ble_enrollment_supported(void);
esp_err_t tater_ble_enrollment_start(
    const char *enrollment_id,
    const char *display_name,
    uint32_t timeout_s
);
void tater_ble_enrollment_cancel(const char *enrollment_id);
bool tater_ble_enrollment_take_event(tater_ble_enrollment_event_t *event);
