#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *context;
    bool (*transfer)(void *context, uint8_t *frame, size_t length);
    void (*wait)(void *context);
    void (*status)(void *context, const uint8_t *frame, size_t length);
} sat1_control_io_t;

/* Caller serializes the entire command and its follow-up response. Read data
 * excludes the servicer status byte, for both protocol 0x10 and 0x11. */
bool sat1_control_exchange(const sat1_control_io_t *io, uint8_t resource,
                           uint8_t command, uint8_t *payload, size_t length);
