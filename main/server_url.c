#include "server_url.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TATER_SERVER_URL_WORK_LEN 256

static bool copy_trimmed(const char *input, char *out, size_t out_len)
{
    if (!input || !out || out_len == 0) {
        return false;
    }

    while (*input && isspace((unsigned char)*input)) {
        input++;
    }
    const char *end = input + strlen(input);
    while (end > input && isspace((unsigned char)end[-1])) {
        end--;
    }

    size_t len = (size_t)(end - input);
    if (len == 0 || len >= out_len) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, input, len);
    out[len] = '\0';
    return true;
}

static bool server_authority_is_valid(const char *value)
{
    if (!value || !value[0]) {
        return false;
    }

    const char *end = strchr(value, '/');
    if (!end) {
        end = value + strlen(value);
    }
    if (end == value || value[0] == ':') {
        return false;
    }

    for (const char *cursor = value; cursor < end; cursor++) {
        if (iscntrl((unsigned char)*cursor) || isspace((unsigned char)*cursor)) {
            return false;
        }
    }
    if (value[0] == '[' && memchr(value, ']', (size_t)(end - value)) == NULL) {
        return false;
    }
    return true;
}

bool tater_server_normalize_base_url(const char *input, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    char value[TATER_SERVER_URL_WORK_LEN];
    if (!copy_trimmed(input, value, sizeof(value))) {
        return false;
    }

    char *query_or_fragment = strpbrk(value, "?#");
    if (query_or_fragment) {
        *query_or_fragment = '\0';
    }

    const char *scheme = "http://";
    const char *server = value;
    if (strncasecmp(value, "http://", 7) == 0) {
        server = value + 7;
    } else if (strncasecmp(value, "https://", 8) == 0) {
        scheme = "https://";
        server = value + 8;
    } else if (strncasecmp(value, "ws://", 5) == 0) {
        server = value + 5;
    } else if (strncasecmp(value, "wss://", 6) == 0) {
        scheme = "https://";
        server = value + 6;
    } else if (strstr(value, "://") != NULL) {
        return false;
    }

    while (*server == '/') {
        server++;
    }
    if (!server_authority_is_valid(server) || strstr(server, "://") != NULL) {
        return false;
    }

    for (const char *cursor = server; *cursor; cursor++) {
        if (iscntrl((unsigned char)*cursor) || isspace((unsigned char)*cursor)) {
            return false;
        }
    }

    char normalized[TATER_SERVER_URL_WORK_LEN];
    int written = snprintf(normalized, sizeof(normalized), "%s%s", scheme, server);
    if (written < 0 || (size_t)written >= sizeof(normalized)) {
        return false;
    }

    char *native_path = strstr(normalized, TATER_NATIVE_WS_PATH);
    if (native_path) {
        const char *after_path = native_path + strlen(TATER_NATIVE_WS_PATH);
        if (*after_path == '\0' || *after_path == '/') {
            *native_path = '\0';
        }
    }

    size_t len = strlen(normalized);
    while (len > 0 && normalized[len - 1] == '/') {
        normalized[--len] = '\0';
    }
    if (len <= strlen(scheme)) {
        return false;
    }

    written = snprintf(out, out_len, "%s", normalized);
    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool tater_server_build_ws_url(const char *input, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    char base[TATER_SERVER_URL_WORK_LEN];
    if (!tater_server_normalize_base_url(input, base, sizeof(base))) {
        return false;
    }

    const char *scheme = NULL;
    const char *server = NULL;
    if (strncmp(base, "http://", 7) == 0) {
        scheme = "ws://";
        server = base + 7;
    } else if (strncmp(base, "https://", 8) == 0) {
        scheme = "wss://";
        server = base + 8;
    } else {
        return false;
    }

    int written = snprintf(out, out_len, "%s%s%s", scheme, server, TATER_NATIVE_WS_PATH);
    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return false;
    }
    return true;
}
