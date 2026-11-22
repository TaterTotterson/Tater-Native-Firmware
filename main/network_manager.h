#pragma once

#include "esp_err.h"
#include "network_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called from startup before creating Wi-Fi/AP/USB interfaces. Idempotent. */
esp_err_t tater_network_init(void);
void tater_network_snapshot(tater_network_state_t *state);
bool tater_network_is_connected(void);
void tater_network_set_ethernet(bool link, uint32_t ip);

#ifdef __cplusplus
}
#endif
