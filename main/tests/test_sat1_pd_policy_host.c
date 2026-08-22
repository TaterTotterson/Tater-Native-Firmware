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
    assert(sat1_pd_select_beta_9v_pdo(supported, 5, &selection));
    assert(selection.object_position == 4);
    assert(selection.voltage_mv == 9000);
    assert(selection.current_ma == 3000);

    const uint32_t no_exact_9v[] = {
        fixed_pdo(5000, 3000),
        fixed_pdo(12000, 3000),
        fixed_pdo(20000, 5000),
    };
    assert(!sat1_pd_select_beta_9v_pdo(no_exact_9v, 3, &selection));

    const uint32_t non_fixed_9v = fixed_pdo(9000, 3000) | (3U << 30);
    assert(!sat1_pd_select_beta_9v_pdo(&non_fixed_9v, 1, &selection));
    assert(!sat1_pd_select_beta_9v_pdo(supported, 8, &selection));

    puts("sat1 PD policy host tests passed");
    return 0;
}
