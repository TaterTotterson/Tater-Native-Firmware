#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../doa_activity_gate.h"

static void calibrate(tater_doa_activity_gate_t *gate, uint32_t mean_abs)
{
    for (int i = 0; i < 8; i++) {
        assert(!tater_doa_activity_gate_update(gate, mean_abs * 4U, mean_abs));
    }
}

int main(void)
{
    tater_doa_activity_gate_t gate;
    tater_doa_activity_gate_reset(&gate);
    calibrate(&gate, 20);
    assert(tater_doa_activity_gate_noise_floor(&gate) == 20);

    for (int i = 0; i < 20; i++) {
        assert(!tater_doa_activity_gate_update(&gate, 100, 22));
    }

    /* A one-frame click must not turn the direction display on. */
    assert(!tater_doa_activity_gate_update(&gate, 2000, 50));
    assert(!tater_doa_activity_gate_update(&gate, 100, 22));

    /* Sustained speech attacks in two frames. */
    assert(!tater_doa_activity_gate_update(&gate, 1200, 180));
    assert(tater_doa_activity_gate_update(&gate, 1200, 180));
    assert(tater_doa_activity_gate_update(&gate, 900, 140));

    /* Brief dips are held; three quiet frames release the gate. */
    assert(tater_doa_activity_gate_update(&gate, 100, 20));
    assert(tater_doa_activity_gate_update(&gate, 100, 20));
    assert(!tater_doa_activity_gate_update(&gate, 100, 20));

    /* A slowly changing room floor adapts without presenting as speech. */
    for (int i = 0; i < 80; i++) {
        assert(!tater_doa_activity_gate_update(&gate, 150, 35));
    }
    assert(tater_doa_activity_gate_noise_floor(&gate) > 20);

    tater_doa_activity_gate_reset(&gate);
    assert(!gate.active);
    assert(tater_doa_activity_gate_noise_floor(&gate) == 0);
    assert(!tater_doa_activity_gate_update(NULL, 1000, 1000));

    puts("DoA activity gate host tests passed.");
    return 0;
}
