#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TATER_WIFI_RETRY_MAX_DELAY_MS (30000U)
#define TATER_WIFI_RETRY_JITTER_MS (250U)

/*
 * The application watchdog is the sole owner of WebSocket reconnects.  These
 * delays keep recovery responsive after a known disconnect without allowing
 * a second connection attempt to overlap the first one's network timeout.
 */
#define TATER_WS_RECONNECT_AFTER_MS (3000U)
#define TATER_WS_RECONNECT_MIN_INTERVAL_MS (5000U)
/* Allow Tater's 10-second server handshake window to finish before recovery. */
#define TATER_WS_HELLO_ACK_TIMEOUT_MS (15000U)
#define TATER_WS_INITIAL_CONNECT_TIMEOUT_MS (20000U)

typedef enum {
    TATER_WS_RECOVERY_NONE = 0,
    TATER_WS_RECOVERY_AUTH_REFRESH,
    TATER_WS_RECOVERY_CLIENT_MISSING,
    TATER_WS_RECOVERY_HELLO_ACK_TIMEOUT,
    TATER_WS_RECOVERY_LINK_DOWN,
    TATER_WS_RECOVERY_INITIAL_CONNECT_TIMEOUT,
} tater_ws_recovery_reason_t;

typedef struct {
    bool lifecycle_restart_active;
    bool client_available;
    bool transport_connected;
    bool hello_acked;
    bool auth_refresh_requested;
    bool link_down_seen;
    uint64_t link_down_age_ms;
    uint64_t client_start_age_ms;
    uint64_t hello_age_ms;
    uint64_t since_attempt_ms;
} tater_ws_recovery_state_t;

uint32_t tater_wifi_retry_delay_ms(uint32_t attempt, uint32_t jitter_seed);
tater_ws_recovery_reason_t tater_ws_recovery_decide(const tater_ws_recovery_state_t *state);
const char *tater_ws_recovery_reason_name(tater_ws_recovery_reason_t reason);

#ifdef __cplusplus
}
#endif
