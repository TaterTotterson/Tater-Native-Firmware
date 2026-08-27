#include "boards/sat1/sat1_pd_policy.h"

#include <string.h>

bool sat1_pd_select_beta_fixed_pdo(
    const uint32_t *objects,
    size_t object_count,
    sat1_pd_fixed_selection_t *selection
)
{
    if (!objects || !selection || object_count == 0 || object_count > 7) {
        return false;
    }

    memset(selection, 0, sizeof(*selection));
    for (size_t index = 0; index < object_count; index++) {
        uint32_t pdo = objects[index];
        if ((pdo >> 30) != 0) {
            continue;
        }
        uint16_t voltage_mv = (uint16_t)(((pdo >> 10) & 0x03ffU) * 50U);
        uint16_t current_ma = (uint16_t)((pdo & 0x03ffU) * 10U);
        if (voltage_mv < SAT1_BETA_MIN_PD_MV || voltage_mv > SAT1_BETA_MAX_PD_MV
            || current_ma == 0) {
            continue;
        }
        if (selection->object_position == 0 || voltage_mv > selection->voltage_mv
            || (voltage_mv == selection->voltage_mv && current_ma > selection->current_ma)) {
            selection->object_position = (uint8_t)(index + 1U);
            selection->voltage_mv = voltage_mv;
            selection->current_ma = current_ma;
        }
    }
    return selection->object_position != 0;
}
