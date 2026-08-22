#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef void (*tater_ota_failure_callback_t)(esp_err_t error);

esp_err_t tater_ota_start_url(const char *url, tater_ota_failure_callback_t failure_callback);
bool tater_ota_is_running(void);
