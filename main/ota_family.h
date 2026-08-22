#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TATER_OTA_FAMILY_MARKER_PREFIX "TATER-OTA-FAMILY:"
#define TATER_OTA_FAMILY_MARKER_SUFFIX ";"

typedef struct {
    size_t matched;
    bool found;
} tater_ota_family_scan_t;

void tater_ota_family_scan_init(tater_ota_family_scan_t *scan);
void tater_ota_family_scan_feed(
    tater_ota_family_scan_t *scan,
    const char *expected_marker,
    const uint8_t *data,
    size_t data_len
);
bool tater_ota_family_scan_found(const tater_ota_family_scan_t *scan);
