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

    tater_ws_recovery_state_t ws = {
        .client_available = true,
        .transport_connected = true,
        .hello_acked = true,
        .since_attempt_ms = UINT64_MAX,
    };
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_NONE);

    ws.lifecycle_restart_active = true;
    ws.auth_refresh_requested = true;
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_NONE);
    ws.lifecycle_restart_active = false;
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_AUTH_REFRESH);

    ws.auth_refresh_requested = false;
    ws.client_available = false;
    ws.transport_connected = false;
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_CLIENT_MISSING);

    ws.client_available = true;
    ws.transport_connected = true;
    ws.hello_acked = false;
    ws.hello_age_ms = TATER_WS_HELLO_ACK_TIMEOUT_MS - 1U;
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_NONE);
    ws.hello_age_ms = TATER_WS_HELLO_ACK_TIMEOUT_MS;
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_HELLO_ACK_TIMEOUT);

    ws.transport_connected = false;
    ws.link_down_seen = true;
    ws.link_down_age_ms = TATER_WS_RECONNECT_AFTER_MS - 1U;
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_NONE);
    ws.link_down_age_ms = TATER_WS_RECONNECT_AFTER_MS;
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_LINK_DOWN);

    ws.link_down_seen = false;
    ws.client_start_age_ms = TATER_WS_INITIAL_CONNECT_TIMEOUT_MS - 1U;
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_NONE);
    ws.client_start_age_ms = TATER_WS_INITIAL_CONNECT_TIMEOUT_MS;
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_INITIAL_CONNECT_TIMEOUT);

    ws.since_attempt_ms = TATER_WS_RECONNECT_MIN_INTERVAL_MS - 1U;
    assert(tater_ws_recovery_decide(&ws) == TATER_WS_RECOVERY_NONE);

    assert(tater_ws_recovery_decide(NULL) == TATER_WS_RECOVERY_NONE);
    assert(tater_ws_recovery_reason_name(TATER_WS_RECOVERY_LINK_DOWN)[0] != '\0');

    puts("Network recovery host tests passed.");
    return 0;
}
