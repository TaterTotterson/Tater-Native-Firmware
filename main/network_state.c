#include "network_state.h"

static void select_route(tater_network_state_t *state)
{
    tater_network_interface_t active = TATER_NETWORK_NONE;
    uint32_t ip = 0;
    if (state->ethernet_link && state->ethernet_ip) {
        active = TATER_NETWORK_USB_ETHERNET;
        ip = state->ethernet_ip;
    } else if (state->wifi_ip) {
        active = TATER_NETWORK_WIFI;
        ip = state->wifi_ip;
    }
    if (active != state->active || ip != state->active_ip) {
        state->active = active;
        state->active_ip = ip;
        ++state->generation;
    }
}

void tater_network_state_wifi(tater_network_state_t *state, uint32_t ip)
{
    state->wifi_ip = ip;
    select_route(state);
}

void tater_network_state_ethernet(tater_network_state_t *state, bool link, uint32_t ip)
{
    state->ethernet_link = link;
    state->ethernet_ip = link ? ip : 0;
    select_route(state);
}

const char *tater_network_interface_name(tater_network_interface_t interface)
{
    switch (interface) {
    case TATER_NETWORK_WIFI: return "wifi";
    case TATER_NETWORK_USB_ETHERNET: return "usb_ethernet";
    default: return "none";
    }
}

bool tater_usb_ethernet_configuration(uint16_t vid, uint16_t pid, uint8_t count, uint8_t *configuration)
{
    if (!configuration || vid != 0x0bda || pid != 0x8152 || count < 2) {
        return false;
    }
    *configuration = 2;
    return true;
}
