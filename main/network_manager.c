#include "network_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

static tater_network_state_t s_state;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;

static void apply_default_route(void)
{
#if CONFIG_TATER_USB_ETHERNET
    tater_network_state_t state;
    tater_network_snapshot(&state);
    const char *key = state.active == TATER_NETWORK_USB_ETHERNET ? "TATER_USB_ETH"
        : state.active == TATER_NETWORK_WIFI ? "WIFI_STA_DEF" : NULL;
    if (key) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(key);
        if (netif) ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_default_netif(netif));
    }
    // esp-netif's automatic choice considers link-up interfaces without DHCP.
    // Manage both transitions explicitly so an address-less NIC cannot win.
#endif
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    uint32_t ip = 0;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip = ((ip_event_got_ip_t *)data)->ip_info.ip.addr;
    } else if (!((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
                 || (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP))) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    tater_network_state_wifi(&s_state, ip);
    portEXIT_CRITICAL(&s_lock);
    apply_default_route();
}

esp_err_t tater_network_init(void)
{
    /* All callers are on app_main's startup path, before transport workers. */
    if (s_initialized) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_event, NULL);
    if (err != ESP_OK) return err;
    s_initialized = true;
    return ESP_OK;
}

void tater_network_snapshot(tater_network_state_t *state)
{
    portENTER_CRITICAL(&s_lock);
    *state = s_state;
    portEXIT_CRITICAL(&s_lock);
}

bool tater_network_is_connected(void)
{
    tater_network_state_t state;
    tater_network_snapshot(&state);
    return state.active != TATER_NETWORK_NONE;
}

void tater_network_set_ethernet(bool link, uint32_t ip)
{
    portENTER_CRITICAL(&s_lock);
    tater_network_state_ethernet(&s_state, link, ip);
    portEXIT_CRITICAL(&s_lock);
    apply_default_route();
}
