#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Shared wire limit: 63 UTF-8 bytes, no controls; an empty room clears it. */
static inline bool native_identity_text(const char *value, char *out, size_t capacity, bool required)
{
    if (!value || !out || !capacity) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p < 32 || *p == 127) return false;
    }
    while (*value == ' ') ++value;
    size_t length = strlen(value);
    while (length && value[length - 1] == ' ') --length;
    if (length > 63 || length >= capacity || (required && !length)) return false;
    memcpy(out, value, length);
    out[length] = '\0';
    return true;
}
