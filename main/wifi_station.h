#pragma once

#include "esp_err.h"
#include "tater_config.h"

esp_err_t tater_wifi_connect(const tater_config_t *config);
bool tater_wifi_is_connected(void);
