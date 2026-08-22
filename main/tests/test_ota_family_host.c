#include "ota_family.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void feed_text(tater_ota_family_scan_t *scan, const char *marker, const char *text)
{
    tater_ota_family_scan_feed(scan, marker, (const uint8_t *)text, strlen(text));
}

int main(void)
{
    const char *production = "TATER-OTA-FAMILY:satellite1;";
    const char *legacy = "TATER-OTA-FAMILY:satellite1-beta-rev41;";

    tater_ota_family_scan_t scan;
    tater_ota_family_scan_init(&scan);
    feed_text(&scan, production, "binary-prefix-TATER-OTA-FAMILY:satellite1;binary-suffix");
    assert(tater_ota_family_scan_found(&scan));

    tater_ota_family_scan_init(&scan);
    feed_text(&scan, legacy, "binary-prefix-TATER-OTA-");
    feed_text(&scan, legacy, "FAMILY:satellite1-beta-");
    feed_text(&scan, legacy, "rev41;binary-suffix");
    assert(tater_ota_family_scan_found(&scan));

    tater_ota_family_scan_init(&scan);
    feed_text(&scan, legacy, "TATER-OTA-FAMILY:satellite1;");
    assert(!tater_ota_family_scan_found(&scan));

    tater_ota_family_scan_init(&scan);
    feed_text(&scan, production, "TATER-OTA-FAMILY:satellite1-beta-rev41;");
    assert(!tater_ota_family_scan_found(&scan));

    tater_ota_family_scan_init(&scan);
    feed_text(&scan, "ABABAC", "ABABABABAC");
    assert(tater_ota_family_scan_found(&scan));

    puts("ota family host tests passed");
    return 0;
}
