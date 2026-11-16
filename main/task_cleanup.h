#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tater_task_cleanup_init(void);
/* Finish a transient task without allocating an IDF self-deletion helper. */
void tater_task_delete_current(bool task_with_caps);

#ifdef __cplusplus
}
#endif
