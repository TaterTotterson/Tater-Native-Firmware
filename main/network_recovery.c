#include "network_recovery.h"

#define TATER_WIFI_RETRY_INITIAL_DELAY_MS (250U)

uint32_t tater_wifi_retry_delay_ms(uint32_t attempt, uint32_t jitter_seed)
{
    if (attempt <= 1U) {
        return 0;
    }

    uint32_t delay_ms = TATER_WIFI_RETRY_INITIAL_DELAY_MS;
    uint32_t doublings = attempt - 2U;
    while (doublings > 0U && delay_ms < TATER_WIFI_RETRY_MAX_DELAY_MS) {
        if (delay_ms > TATER_WIFI_RETRY_MAX_DELAY_MS / 2U) {
            delay_ms = TATER_WIFI_RETRY_MAX_DELAY_MS;
            break;
        }
        delay_ms *= 2U;
        doublings--;
    }

    uint32_t jitter_ms = jitter_seed % (TATER_WIFI_RETRY_JITTER_MS + 1U);
    if (delay_ms >= TATER_WIFI_RETRY_MAX_DELAY_MS - jitter_ms) {
        return TATER_WIFI_RETRY_MAX_DELAY_MS;
    }
    return delay_ms + jitter_ms;
}
