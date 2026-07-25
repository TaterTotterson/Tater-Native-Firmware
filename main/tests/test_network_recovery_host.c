#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../network_recovery.h"

int main(void)
{
    assert(tater_wifi_retry_delay_ms(0, 0) == 0);
    assert(tater_wifi_retry_delay_ms(1, 250) == 0);
    assert(tater_wifi_retry_delay_ms(2, 0) == 250);
    assert(tater_wifi_retry_delay_ms(2, 250) == 500);
    assert(tater_wifi_retry_delay_ms(3, 0) == 500);
    assert(tater_wifi_retry_delay_ms(4, 0) == 1000);
    assert(tater_wifi_retry_delay_ms(8, 0) == 16000);
    assert(tater_wifi_retry_delay_ms(9, 0) == TATER_WIFI_RETRY_MAX_DELAY_MS);
    assert(tater_wifi_retry_delay_ms(UINT32_MAX, UINT32_MAX) == TATER_WIFI_RETRY_MAX_DELAY_MS);

    uint32_t previous = 0;
    for (uint32_t attempt = 1; attempt < 100; attempt++) {
        uint32_t delay = tater_wifi_retry_delay_ms(attempt, attempt * 17U);
        assert(delay <= TATER_WIFI_RETRY_MAX_DELAY_MS);
        assert(delay >= previous || delay == TATER_WIFI_RETRY_MAX_DELAY_MS);
        previous = delay;
    }

    puts("Network recovery host tests passed.");
    return 0;
}
