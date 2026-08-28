#include "boards/sat1/sat1_pd_policy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t fixed_pdo(uint16_t voltage_mv, uint16_t current_ma)
{
    return ((uint32_t)(voltage_mv / 50U) << 10) | (current_ma / 10U);
}

int main(void)
{
    sat1_pd_fixed_selection_t selection = {0};
    const uint32_t supported[] = {
        fixed_pdo(5000, 3000),
        fixed_pdo(9000, 2000),
        fixed_pdo(15000, 3000),
        fixed_pdo(9000, 3000),
        fixed_pdo(20000, 5000),
    };
    assert(sat1_pd_select_fixed_pdo(supported, 5, &selection));
    assert(selection.object_position == 5);
    assert(selection.voltage_mv == 20000);
    assert(selection.current_ma == 5000);

    const uint32_t no_20v[] = {
        fixed_pdo(5000, 3000),
        fixed_pdo(12000, 3000),
        fixed_pdo(15000, 2500),
    };
    assert(sat1_pd_select_fixed_pdo(no_20v, 3, &selection));
    assert(selection.object_position == 3);
    assert(selection.voltage_mv == 15000);
    assert(selection.current_ma == 2500);

    const uint32_t duplicate_voltage[] = {
        fixed_pdo(5000, 3000),
        fixed_pdo(15000, 2000),
        fixed_pdo(15000, 3000),
    };
    assert(sat1_pd_select_fixed_pdo(duplicate_voltage, 3, &selection));
    assert(selection.object_position == 3);
    assert(selection.voltage_mv == 15000);
    assert(selection.current_ma == 3000);

    const uint32_t over_limit[] = {
        fixed_pdo(5000, 3000),
        fixed_pdo(21000, 3000),
    };
    assert(sat1_pd_select_fixed_pdo(over_limit, 2, &selection));
    assert(selection.object_position == 1);
    assert(selection.voltage_mv == 5000);
    assert(selection.current_ma == 3000);

    const uint32_t non_fixed_9v = fixed_pdo(9000, 3000) | (3U << 30);
    assert(!sat1_pd_select_fixed_pdo(&non_fixed_9v, 1, &selection));

    const uint32_t invalid_fixed[] = {
        fixed_pdo(4500, 3000),
        fixed_pdo(9000, 0),
    };
    assert(!sat1_pd_select_fixed_pdo(invalid_fixed, 2, &selection));
    assert(!sat1_pd_select_fixed_pdo(supported, 8, &selection));

    puts("sat1 PD policy host tests passed");
    return 0;
}
