#include "boards/sat1/sat1_control_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned transfers, waits, statuses;
    bool modern, write, fail, ignored_request, ignored_poll, command_error, nop, always_ignored;
    uint8_t pending;
} fake_t;

static bool transfer(void *context, uint8_t *frame, size_t length)
{
    fake_t *f = context;
    ++f->transfers;
    assert(length <= 256 && length >= 8);
    if (f->fail) return false;
    if (f->always_ignored) { memset(frame, 0, length); frame[0] = 7; return true; }
    if (f->nop) { assert(frame[0] == 0 && frame[1] == 0 && frame[2] == 0);
        memset(frame, 0, length); frame[0] = 1; frame[2] = 1; return true; }
    if (frame[0] != 0) {
        assert(frame[0] == 233);
        if (f->write) {
            assert(frame[1] == 2 && frame[2] == 1 && frame[3] == 4);
        } else {
            assert(frame[1] == 0x80 && frame[2] == 6);
        }
        memset(frame, 0, length);
        if (f->ignored_request) { frame[0] = 7; f->ignored_request = false; }
        else { frame[0] = 1; f->pending = 1; }
    } else {
        assert(frame[1] == 0 && frame[2] == 0);
        memset(frame, 0, length);
        if (f->ignored_poll) { frame[0] = 7; f->ignored_poll = false; return true; }
        if (f->write) {
            frame[0] = 1; frame[1] = f->command_error ? 2 : 0;
        } else {
            size_t offset = f->modern ? 1 : 0;
            if (f->modern) frame[0] = 23;
            frame[offset] = f->command_error ? 2 : 0;
            const uint8_t version[] = {1, 2, 0, 4, 3};
            memcpy(frame + offset + 1, version, sizeof(version));
        }
        f->pending = 0;
    }
    return true;
}
static void wait(void *context) { ++((fake_t *)context)->waits; }
static void status(void *context, const uint8_t *frame, size_t length)
{ (void)frame; assert(length >= 8); ++((fake_t *)context)->statuses; }
int main(void)
{
    for (unsigned modern = 0; modern < 2; ++modern) {
        for (unsigned retry = 0; retry < 2; ++retry) {
            fake_t f = {.modern = modern, .ignored_request = retry, .ignored_poll = retry};
            sat1_control_io_t io = {&f, transfer, wait, status};
            uint8_t version[5] = {0};
            const uint8_t expected[] = {1, 2, 0, 4, 3};
            assert(sat1_control_exchange(&io, 233, 0x80, version, sizeof(version)));
            assert(memcmp(version, expected, sizeof(version)) == 0);
            assert(f.transfers == (retry ? 4u : 2u) && f.pending == 0);
            assert(f.waits == f.transfers && f.statuses == f.transfers);
            f.command_error = true;
            memset(version, 0xa5, sizeof(version));
            assert(!sat1_control_exchange(&io, 233, 0x80, version, sizeof(version)));
            assert(version[0] == 0xa5);
            f.write = true; f.command_error = false;
            uint8_t flags = 4;
            assert(sat1_control_exchange(&io, 233, 2, &flags, 1));
            f.command_error = true;
            assert(!sat1_control_exchange(&io, 233, 2, &flags, 1));
            f.fail = true;
            assert(!sat1_control_exchange(&io, 233, 2, &flags, 1));
            assert(!sat1_control_exchange(&io, 233, 0x80, NULL, 5));
            assert(!sat1_control_exchange(&io, 233, 0x80, version, 252));
        }
    }
    fake_t f = {.nop = true};
    sat1_control_io_t io = {&f, transfer, wait, status};
    assert(sat1_control_exchange(&io, 0, 0, NULL, 0));
    assert(f.transfers == 1 && f.statuses == 1);
    f.nop = false; f.always_ignored = true; f.transfers = 0;
    uint8_t flags = 4;
    assert(!sat1_control_exchange(&io, 233, 2, &flags, 1));
    assert(f.transfers == 4);
    puts("Sat1 control protocol 0x10/0x11 host tests passed.");
    return 0;
}
