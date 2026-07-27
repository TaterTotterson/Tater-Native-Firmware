#include "timer_sound_assets.h"

extern const uint8_t _binary_zen_timer_alarm_wav_start[] asm("_binary_zen_timer_alarm_wav_start");
extern const uint8_t _binary_zen_timer_alarm_wav_end[] asm("_binary_zen_timer_alarm_wav_end");

static const tater_timer_sound_asset_t s_timer_alarm = {
    .data = _binary_zen_timer_alarm_wav_start,
    .end = _binary_zen_timer_alarm_wav_end,
};

const tater_timer_sound_asset_t *tater_timer_sound_asset(void)
{
    return &s_timer_alarm;
}
