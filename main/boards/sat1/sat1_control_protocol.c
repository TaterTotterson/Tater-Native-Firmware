#include "sat1_control_protocol.h"

#include <string.h>

#define READ_COMMAND 0x80u
#define IGNORED_COMMAND 7u
#define PAYLOAD_AVAILABLE 23u
#define ATTEMPTS 4

bool sat1_control_exchange(const sat1_control_io_t *io, uint8_t resource,
                           uint8_t command, uint8_t *payload, size_t length)
{
    bool read = (command & READ_COMMAND) != 0;
    if (io == NULL || io->transfer == NULL || io->wait == NULL ||
        (length != 0 && payload == NULL) || length > (read ? 251u : 253u)) {
        return false;
    }

    uint8_t frame[256];
    size_t wire_length = length + (read ? 1u : 0u);
    size_t request_length = wire_length + 3u;
    if (request_length < 8) request_length = 8;
    bool accepted = false;
    for (unsigned attempt = 0; attempt < ATTEMPTS; ++attempt) {
        memset(frame, 0, sizeof(frame));
        frame[0] = resource;
        frame[1] = command;
        frame[2] = (uint8_t)wire_length;
        if (!read && length != 0) memcpy(frame + 3, payload, length);
        if (!io->transfer(io->context, frame, request_length)) return false;
        if (io->status != NULL) io->status(io->context, frame, request_length);
        io->wait(io->context);
        if (frame[0] != IGNORED_COMMAND) {
            accepted = true;
            break;
        }
    }
    if (!accepted) return false;

    /* A NOP already clocks the previously staged status. */
    if (resource == 0 && command == 0 && length == 0) {
        return frame[0] == 1 && frame[1] == 0;
    }

    size_t response_length = length + 2u;
    if (response_length < 8) response_length = 8;
    for (unsigned attempt = 0; attempt < ATTEMPTS; ++attempt) {
        memset(frame, 0, sizeof(frame));
        if (!io->transfer(io->context, frame, response_length)) return false;
        if (io->status != NULL) io->status(io->context, frame, response_length);
        io->wait(io->context);
        if (frame[0] == IGNORED_COMMAND) continue;
        if (!read) return frame[0] == 1 && frame[1] == 0;

        size_t status_offset = frame[0] == PAYLOAD_AVAILABLE ? 1u : 0u;
        if (frame[status_offset] != 0) return false;
        if (length != 0) memcpy(payload, frame + status_offset + 1u, length);
        return true;
    }
    return false;
}
