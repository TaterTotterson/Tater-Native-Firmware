#pragma once

#include <stdbool.h>
#include <stddef.h>

#define TATER_NATIVE_WS_PATH "/api/tater/satellite/v1/ws"

bool tater_server_normalize_base_url(const char *input, char *out, size_t out_len);
bool tater_server_build_ws_url(const char *input, char *out, size_t out_len);
