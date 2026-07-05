#pragma once

#include "esp_err.h"
#include "tater_config.h"

esp_err_t tater_provisioning_start(const tater_config_t *initial);
const char *tater_provisioning_ssid(void);
