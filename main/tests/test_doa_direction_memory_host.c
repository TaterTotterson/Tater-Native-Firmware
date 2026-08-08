#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../doa_direction_memory.h"

int main(void)
{
    tater_doa_direction_memory_t memory;
    uint8_t led = 0;

    tater_doa_direction_memory_reset(&memory, 12, 6);
    assert(!tater_doa_direction_memory_dominant(&memory, &led));
    assert(led == 6);

    /* A few strong outliers must not beat the direction seen most often. */
    for (int i = 0; i < 3; i++) {
        tater_doa_direction_memory_observe(&memory, 9.0f, 255);
    }
    for (int i = 0; i < 8; i++) {
        tater_doa_direction_memory_observe(&memory, 2.0f, 12);
    }
    assert(tater_doa_direction_memory_dominant(&memory, &led));
    assert(led == 2);

    /* Circular positions and their neighbor votes stay on the correct edge. */
    tater_doa_direction_memory_reset(&memory, 12, 6);
    for (int i = 0; i < 5; i++) {
        tater_doa_direction_memory_observe(&memory, 11.8f, 32);
    }
    assert(tater_doa_direction_memory_dominant(&memory, &led));
    assert(led == 0);

    tater_doa_direction_memory_reset(&memory, 24, 12);
    assert(!tater_doa_direction_memory_dominant(&memory, &led));
    assert(led == 12);
    tater_doa_direction_memory_observe(&memory, -1.0f, 20);
    assert(tater_doa_direction_memory_dominant(&memory, &led));
    assert(led == 23);

    tater_doa_direction_memory_reset(&memory, 0, 0);
    assert(!tater_doa_direction_memory_dominant(&memory, &led));
    tater_doa_direction_memory_observe(NULL, 1.0f, 1);

    puts("DoA direction memory host tests passed.");
    return 0;
}
