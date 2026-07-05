#pragma once

#include "esp_err.h"
#include "tater_protocol.h"

esp_err_t tater_leds_init(void);
void tater_leds_set_state(tater_state_t state);
void tater_leds_set_brightness(uint8_t brightness);
void tater_leds_show_setup_reset_clicks(uint8_t clicks, uint8_t required_clicks);
void tater_leds_show_setup_reset_countdown(uint8_t remaining_steps, uint8_t total_steps);
void tater_leds_show_setup_reset_success(void);
void tater_leds_clear_setup_reset_feedback(void);
