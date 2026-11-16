#include "room_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool is_ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static bool is_continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

/*
 * Validates [cursor, end) as well-formed UTF-8 (RFC 3629) without C0 control
 * characters or DEL. Overlong forms, UTF-16 surrogates, and code points above
 * U+10FFFF are rejected by restricting the lead and second byte ranges.
 */
static tater_room_config_result_t validate_text(const unsigned char *cursor, const unsigned char *end)
{
    while (cursor < end) {
        unsigned char lead = *cursor;
        if (lead < 0x80) {
            if (lead < 0x20 || lead == 0x7F) {
                return TATER_ROOM_CONFIG_CONTROL_CHARACTER;
            }
            cursor++;
            continue;
        }

        size_t length = 0;
        unsigned char second_min = 0x80;
        unsigned char second_max = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead == 0xE0) {
            length = 3;
            second_min = 0xA0;
        } else if (lead == 0xED) {
            length = 3;
            second_max = 0x9F;
        } else if (lead >= 0xE1 && lead <= 0xEF) {
            length = 3;
        } else if (lead == 0xF0) {
            length = 4;
            second_min = 0x90;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            length = 4;
        } else if (lead == 0xF4) {
            length = 4;
            second_max = 0x8F;
        } else {
            // 0x80-0xC1 (continuation or overlong 2-byte lead) and 0xF5-0xFF.
            return TATER_ROOM_CONFIG_INVALID_UTF8;
        }

        if ((size_t)(end - cursor) < length) {
            return TATER_ROOM_CONFIG_INVALID_UTF8;
        }
        if (cursor[1] < second_min || cursor[1] > second_max) {
            return TATER_ROOM_CONFIG_INVALID_UTF8;
        }
        for (size_t i = 2; i < length; i++) {
            if (!is_continuation(cursor[i])) {
                return TATER_ROOM_CONFIG_INVALID_UTF8;
            }
        }
        cursor += length;
    }
    return TATER_ROOM_CONFIG_OK;
}

tater_room_config_result_t tater_room_config_normalize(const char *input, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return TATER_ROOM_CONFIG_INVALID_ARGUMENT;
    }
    out[0] = '\0';
    if (!input) {
        return TATER_ROOM_CONFIG_INVALID_TYPE;
    }

    const unsigned char *start = (const unsigned char *)input;
    while (*start && is_ascii_space(*start)) {
        start++;
    }
    const unsigned char *end = start + strlen((const char *)start);
    while (end > start && is_ascii_space(end[-1])) {
        end--;
    }

    size_t len = (size_t)(end - start);
    if (len >= out_len) {
        return TATER_ROOM_CONFIG_TOO_LONG;
    }
    tater_room_config_result_t result = validate_text(start, end);
    if (result != TATER_ROOM_CONFIG_OK) {
        return result;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return TATER_ROOM_CONFIG_OK;
}

bool tater_room_config_json_has_nul_escape(const char *json, size_t len)
{
    if (!json) {
        return false;
    }
    size_t backslashes = 0;
    for (size_t i = 0; i < len; i++) {
        if (json[i] == '\\') {
            backslashes++;
            continue;
        }
        if ((backslashes & 1u) && json[i] == 'u' && len - i >= 5 && memcmp(&json[i + 1], "0000", 4) == 0) {
            return true;
        }
        backslashes = 0;
    }
    return false;
}

const char *tater_room_config_result_code(tater_room_config_result_t result)
{
    switch (result) {
    case TATER_ROOM_CONFIG_OK:
        return "ok";
    case TATER_ROOM_CONFIG_INVALID_ARGUMENT:
        return "invalid_argument";
    case TATER_ROOM_CONFIG_INVALID_TYPE:
        return "invalid_type";
    case TATER_ROOM_CONFIG_TOO_LONG:
        return "too_long";
    case TATER_ROOM_CONFIG_CONTROL_CHARACTER:
        return "control_character";
    case TATER_ROOM_CONFIG_INVALID_UTF8:
        return "invalid_utf8";
    default:
        return "unknown_error";
    }
}

const char *tater_room_config_result_message(tater_room_config_result_t result)
{
    switch (result) {
    case TATER_ROOM_CONFIG_OK:
        return "";
    case TATER_ROOM_CONFIG_INVALID_ARGUMENT:
        return "Room could not be processed.";
    case TATER_ROOM_CONFIG_INVALID_TYPE:
        return "Room must be a string.";
    case TATER_ROOM_CONFIG_TOO_LONG:
        return "Room name is too long.";
    case TATER_ROOM_CONFIG_CONTROL_CHARACTER:
        return "Room name contains a control character.";
    case TATER_ROOM_CONFIG_INVALID_UTF8:
        return "Room name is not valid UTF-8.";
    default:
        return "Room could not be processed.";
    }
}
