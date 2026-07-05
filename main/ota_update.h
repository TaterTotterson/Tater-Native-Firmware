#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t tater_ota_start_url(const char *url);
bool tater_ota_is_running(void);
