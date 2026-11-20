#pragma once
#include <stdbool.h>
#include "cJSON.h"

typedef void (*tater_ble_proxy_send_t)(const char *type, cJSON *payload);
void tater_ble_proxy_init(tater_ble_proxy_send_t send);
void tater_ble_proxy_request(const char *message_id, const cJSON *payload);
void tater_ble_proxy_poll(void);
void tater_ble_proxy_reset(void);
void tater_ble_proxy_host_config(void);

void tater_ble_proxy_host_reset(void);
