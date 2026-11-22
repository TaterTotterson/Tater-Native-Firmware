#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TATER_NETWORK_NONE,
    TATER_NETWORK_WIFI,
    TATER_NETWORK_USB_ETHERNET,
} tater_network_interface_t;

typedef struct {
    uint32_t wifi_ip;
    uint32_t ethernet_ip;
    bool ethernet_link;
    tater_network_interface_t active;
    uint32_t active_ip;
    uint32_t generation;
} tater_network_state_t;

/* IP addresses use esp-netif's network byte order. Zero means unavailable. */
void tater_network_state_wifi(tater_network_state_t *state, uint32_t ip);
void tater_network_state_ethernet(tater_network_state_t *state, bool link, uint32_t ip);
const char *tater_network_interface_name(tater_network_interface_t interface);

/* Restrict this prototype to the existing RTL8152 adapter, using its ECM config. */
bool tater_usb_ethernet_configuration(uint16_t vid, uint16_t pid, uint8_t count, uint8_t *configuration);

#ifdef __cplusplus
}
#endif
