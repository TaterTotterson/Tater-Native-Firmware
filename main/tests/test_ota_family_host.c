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
    const char *voicepe = "TATER-OTA-FAMILY:voice-pe;";

    tater_ota_family_scan_t scan;
    tater_ota_family_scan_init(&scan);
    feed_text(&scan, production, "binary-prefix-TATER-OTA-FAMILY:satellite1;binary-suffix");
    assert(tater_ota_family_scan_found(&scan));

    tater_ota_family_scan_init(&scan);
    feed_text(&scan, voicepe, "binary-prefix-TATER-OTA-");
    feed_text(&scan, voicepe, "FAMILY:voice-");
    feed_text(&scan, voicepe, "pe;binary-suffix");
    assert(tater_ota_family_scan_found(&scan));

    tater_ota_family_scan_init(&scan);
    feed_text(&scan, voicepe, "TATER-OTA-FAMILY:satellite1;");
    assert(!tater_ota_family_scan_found(&scan));

    tater_ota_family_scan_init(&scan);
    feed_text(&scan, production, "TATER-OTA-FAMILY:voice-pe;");
    assert(!tater_ota_family_scan_found(&scan));

    tater_ota_family_scan_init(&scan);
    feed_text(&scan, "ABABAC", "ABABABABAC");
    assert(tater_ota_family_scan_found(&scan));

    puts("ota family host tests passed");
    return 0;
}
