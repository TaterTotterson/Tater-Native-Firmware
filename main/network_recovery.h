#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TATER_WIFI_RETRY_MAX_DELAY_MS (30000U)
#define TATER_WIFI_RETRY_JITTER_MS (250U)

uint32_t tater_wifi_retry_delay_ms(uint32_t attempt, uint32_t jitter_seed);

#ifdef __cplusplus
}
#endif
