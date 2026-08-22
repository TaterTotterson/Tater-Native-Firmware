#include "ota_family.h"

#include <string.h>

static size_t marker_fallback(const char *marker, size_t matched)
{
    for (size_t candidate = matched; candidate > 0; candidate--) {
        size_t fallback = candidate - 1;
        if (fallback == 0
            || memcmp(marker, marker + matched - fallback, fallback) == 0) {
            return fallback;
        }
    }
    return 0;
}

void tater_ota_family_scan_init(tater_ota_family_scan_t *scan)
{
    if (scan) {
        memset(scan, 0, sizeof(*scan));
    }
}

void tater_ota_family_scan_feed(
    tater_ota_family_scan_t *scan,
    const char *expected_marker,
    const uint8_t *data,
    size_t data_len
)
{
    if (!scan || scan->found || !expected_marker || !expected_marker[0] || !data) {
        return;
    }

    size_t marker_len = strlen(expected_marker);
    for (size_t index = 0; index < data_len && !scan->found; index++) {
        uint8_t byte = data[index];
        while (scan->matched > 0 && byte != (uint8_t)expected_marker[scan->matched]) {
            scan->matched = marker_fallback(expected_marker, scan->matched);
        }
        if (byte == (uint8_t)expected_marker[scan->matched]) {
            scan->matched++;
            if (scan->matched == marker_len) {
                scan->found = true;
            }
        }
    }
}

bool tater_ota_family_scan_found(const tater_ota_family_scan_t *scan)
{
    return scan && scan->found;
}
