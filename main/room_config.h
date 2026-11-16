#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Pure validation for a controller-supplied room name. No ESP-IDF
 * dependencies so it can be exercised by the host tests.
 */
typedef enum {
    TATER_ROOM_CONFIG_OK = 0,
    TATER_ROOM_CONFIG_INVALID_ARGUMENT,
    TATER_ROOM_CONFIG_INVALID_TYPE,
    TATER_ROOM_CONFIG_TOO_LONG,
    TATER_ROOM_CONFIG_CONTROL_CHARACTER,
    TATER_ROOM_CONFIG_INVALID_UTF8,
} tater_room_config_result_t;

/*
 * Trims leading/trailing ASCII whitespace and validates the remainder as a
 * room name that fits in out (out_len includes the NUL). An empty result is
 * valid and means "no room". Input that does not fit is rejected rather than
 * truncated so a multibyte UTF-8 sequence is never split. On any failure out
 * is set to the empty string when it is writable.
 */
tater_room_config_result_t tater_room_config_normalize(const char *input, char *out, size_t out_len);

/* Stable snake_case code for protocol replies, e.g. "ok" or "too_long". */
const char *tater_room_config_result_code(tater_room_config_result_t result);

/* Short human-readable description; empty for TATER_ROOM_CONFIG_OK. */
const char *tater_room_config_result_message(tater_room_config_result_t result);

/*
 * True when the raw JSON text json[0, len) contains a \u0000 escape (one
 * preceded by an odd run of backslashes, so "\\u0000" does not count).
 * cJSON decodes that escape to an embedded NUL without recording the string
 * length, so a decoded value such as "Kitchen\u0000junk" would otherwise
 * read as "Kitchen" and be silently truncated rather than rejected.
 */
bool tater_room_config_json_has_nul_escape(const char *json, size_t len);
