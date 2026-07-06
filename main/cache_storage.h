#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t tater_cache_init(void);
bool tater_cache_ready(void);
const char *tater_cache_base_path(void);
