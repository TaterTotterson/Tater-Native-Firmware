#pragma once

#include "esp_err.h"

// Run the complete XMOS update, including image decompression, on a temporary
// PSRAM stack. Blocks until the update finishes and its task has been deleted.
esp_err_t tater_xmos_update_run(esp_err_t (*update)(void));
