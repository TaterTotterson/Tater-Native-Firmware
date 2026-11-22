#pragma once

#include "esp_err.h"

esp_err_t tater_usb_ethernet_start(void);
/* Static string; safe to include in remote diagnostics while USB hosts the NIC. */
const char *tater_usb_ethernet_status(void);
