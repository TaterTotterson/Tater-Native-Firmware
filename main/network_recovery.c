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

tater_ws_recovery_reason_t tater_ws_recovery_decide(const tater_ws_recovery_state_t *state)
{
    if (!state || state->lifecycle_restart_active) {
        return TATER_WS_RECOVERY_NONE;
    }

    if (state->transport_connected && state->hello_acked && !state->auth_refresh_requested) {
        return TATER_WS_RECOVERY_NONE;
    }

    if (state->since_attempt_ms < TATER_WS_RECONNECT_MIN_INTERVAL_MS) {
        return TATER_WS_RECOVERY_NONE;
    }

    if (state->auth_refresh_requested) {
        return TATER_WS_RECOVERY_AUTH_REFRESH;
    }

    if (!state->client_available) {
        return TATER_WS_RECOVERY_CLIENT_MISSING;
    }

    if (state->transport_connected) {
        if (!state->hello_acked && state->hello_age_ms >= TATER_WS_HELLO_ACK_TIMEOUT_MS) {
            return TATER_WS_RECOVERY_HELLO_ACK_TIMEOUT;
        }
        return TATER_WS_RECOVERY_NONE;
    }

    if (state->link_down_seen) {
        return state->link_down_age_ms >= TATER_WS_RECONNECT_AFTER_MS
            ? TATER_WS_RECOVERY_LINK_DOWN
            : TATER_WS_RECOVERY_NONE;
    }

    return state->client_start_age_ms >= TATER_WS_INITIAL_CONNECT_TIMEOUT_MS
        ? TATER_WS_RECOVERY_INITIAL_CONNECT_TIMEOUT
        : TATER_WS_RECOVERY_NONE;
}

const char *tater_ws_recovery_reason_name(tater_ws_recovery_reason_t reason)
{
    switch (reason) {
    case TATER_WS_RECOVERY_AUTH_REFRESH:
        return "auth_refresh";
    case TATER_WS_RECOVERY_CLIENT_MISSING:
        return "client_missing";
    case TATER_WS_RECOVERY_HELLO_ACK_TIMEOUT:
        return "hello_ack_timeout";
    case TATER_WS_RECOVERY_LINK_DOWN:
        return "link_down";
    case TATER_WS_RECOVERY_INITIAL_CONNECT_TIMEOUT:
        return "initial_connect_timeout";
    case TATER_WS_RECOVERY_NONE:
    default:
        return "none";
    }
}
