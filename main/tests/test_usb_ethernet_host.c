#include "fake_idf.h"
#include <string.h>
#include "network_manager.h"
#include "usb_ethernet.h"

const char *WIFI_EVENT = "wifi", *IP_EVENT = "ip", *IOT_ETH_EVENT = "eth";
static const char *scenario;
static unsigned host_count, cdc_count, eth_count, glue_count, netif_count, delays;
static unsigned netif_init_count, loop_init_count, task_count;
static esp_netif_t wifi, *wired, *selected;
static iot_eth_handle_t eth_handle;
static usb_host_config_t host_config;
static iot_eth_driver_t *eth_driver;
static void *live_glue;
static unsigned mdns_count;
static bool mdns_enabled;
typedef struct { esp_event_base_t base; int32_t id; event_handler_t fn; bool live; } handler_t;
static handler_t handlers[16];
static unsigned handler_count;
static bool fails(const char *stage) { return strcmp(scenario, stage) == 0; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "fake"; }
esp_err_t esp_netif_init(void) { ++netif_init_count; return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { ++loop_init_count; return ESP_OK; }
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, event_handler_t fn, void *arg)
{
    (void)arg; assert(handler_count < 16);
    handlers[handler_count++] = (handler_t){base, id, fn, true}; return ESP_OK;
}
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id, event_handler_t fn, void *arg, void **instance)
{
    if (fails(base == IOT_ETH_EVENT ? "eth_events" : "ip_events")) return ESP_FAIL;
    *instance = &handlers[handler_count]; return esp_event_handler_register(base, id, fn, arg);
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id, void *instance)
{ (void)base; (void)id; ((handler_t *)instance)->live = false; return ESP_OK; }
static void event(esp_event_base_t base, int32_t id, void *data)
{
    for (unsigned i = 0; i < handler_count; ++i)
        if (handlers[i].live && handlers[i].base == base && (handlers[i].id == id || handlers[i].id == ESP_EVENT_ANY_ID))
            handlers[i].fn(NULL, base, id, data);
}
esp_netif_t *esp_netif_new(const esp_netif_config_t *config)
{
    if (fails("netif")) return NULL;
    assert(strcmp(config->base->if_key, "TATER_USB_ETH") == 0);
    assert(config->base->route_prio > 100);
    ++netif_count; wired = calloc(1, sizeof(*wired)); return wired;
}
void esp_netif_destroy(esp_netif_t *netif) { assert(netif == wired); --netif_count; free(wired); wired = NULL; }
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{ return strcmp(key, "WIFI_STA_DEF") == 0 ? &wifi : wired; }
esp_err_t esp_netif_set_default_netif(esp_netif_t *netif) { selected = netif; return ESP_OK; }
esp_err_t esp_netif_attach(esp_netif_t *netif, void *glue)
{ assert(netif && glue); return fails("attach") ? ESP_FAIL : ESP_OK; }
bool usb_serial_jtag_is_connected(void)
{ return fails("console") || (fails("late_console") && delays > 10); }
void vTaskDelay(uint32_t ms) { (void)ms; ++delays; }
int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, unsigned priority, void *handle, int core)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; (void)core;
    if (fails("task")) return 0;
    ++task_count; return pdPASS;
}
esp_err_t usb_host_install(const usb_host_config_t *config)
{ if (fails("host")) return ESP_FAIL; ++host_count; host_config = *config; return ESP_OK; }
esp_err_t usb_host_uninstall(void) { --host_count; return ESP_OK; }
esp_err_t usb_host_lib_handle_events(uint32_t timeout, uint32_t *flags)
{ (void)timeout; (void)flags; assert(false); return ESP_FAIL; }
esp_err_t usbh_cdc_driver_install(const usbh_cdc_driver_config_t *config)
{ assert(config->skip_init_usb_host_driver); if (fails("cdc")) return ESP_FAIL; ++cdc_count; return ESP_OK; }
esp_err_t usbh_cdc_driver_uninstall(void) { --cdc_count; return ESP_OK; }
esp_err_t iot_eth_new_usb_ecm(const iot_usbh_ecm_config_t *config, iot_eth_driver_t **driver)
{
    assert(config->match_id_list[0].idVendor == 0x0bda && config->match_id_list[0].idProduct == 0x8152);
    if (fails("new_ecm")) return ESP_FAIL;
    *driver = calloc(1, sizeof(**driver)); return ESP_OK;
}
esp_err_t iot_eth_install(const iot_eth_config_t *config, void **handle)
{
    if (fails("eth_install")) return ESP_FAIL;
    eth_driver = config->driver; ++eth_count; *handle = eth_handle = &eth_driver; return ESP_OK;
}
esp_err_t iot_eth_uninstall(void *handle) { assert(handle == eth_handle); --eth_count; free(eth_driver); return ESP_OK; }
esp_err_t iot_eth_start(void *handle) { assert(handle == eth_handle); return fails("eth_start") ? ESP_FAIL : ESP_OK; }
esp_err_t iot_eth_stop(void *handle) { assert(handle == eth_handle); return ESP_OK; }
void *iot_eth_new_netif_glue(void *handle)
{ assert(handle == eth_handle); if (fails("glue")) return NULL; ++glue_count; return live_glue = malloc(1); }
esp_err_t iot_eth_del_netif_glue(void *glue) { --glue_count; free(glue); return ESP_OK; }

esp_err_t mdns_init(void) { return fails("mdns_init") ? ESP_FAIL : ESP_OK; }
esp_err_t mdns_register_netif(esp_netif_t *netif)
{ assert(netif == wired); if (fails("mdns_register")) return ESP_FAIL; ++mdns_count; return ESP_OK; }
esp_err_t mdns_unregister_netif(esp_netif_t *netif)
{ assert(netif == wired); --mdns_count; return ESP_OK; }
esp_err_t mdns_netif_action(esp_netif_t *netif, mdns_event_actions_t action)
{
    assert(netif == wired && mdns_count);
    if (action & MDNS_EVENT_ENABLE_IP4) mdns_enabled = true;
    if (action & MDNS_EVENT_DISABLE_IP4) mdns_enabled = false;
    return ESP_OK;
}

static void expect(tater_network_interface_t active, unsigned generation)
{
    tater_network_state_t state; tater_network_snapshot(&state);
    assert(state.active == active && state.generation == generation);
    assert(tater_network_is_connected() == (active != TATER_NETWORK_NONE));
}
int main(int argc, char **argv)
{
    assert(argc == 2); scenario = argv[1];
    assert(tater_network_init() == ESP_OK && tater_network_init() == ESP_OK);
    assert(netif_init_count == 1 && loop_init_count == 1 && handler_count == 3);
    expect(TATER_NETWORK_NONE, 0);
    ip_event_got_ip_t wifi_ip = { .esp_netif = &wifi, .ip_info.ip.addr = 10 };
    event(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_ip);
    expect(TATER_NETWORK_WIFI, 1); assert(selected == &wifi);
    esp_err_t result = tater_usb_ethernet_start();
    if (fails("console") || fails("late_console")) {
        assert(result == ESP_OK && strcmp(tater_usb_ethernet_status(), "console_preserved") == 0);
        assert(!host_count && !task_count);
    } else if (!fails("success")) {
        assert(result != ESP_OK && strcmp(tater_usb_ethernet_status(), "failed") == 0);
        assert(!host_count && !cdc_count && !eth_count && !netif_count && !glue_count && !task_count && !mdns_count);
        for (unsigned i = 3; i < handler_count; ++i) assert(!handlers[i].live);
    } else {
        assert(result == ESP_OK && task_count == 1 && host_count == 1);
        usb_device_desc_t descriptor = { .idVendor = 0x0bda, .idProduct = 0x8152, .bNumConfigurations = 2 };
        uint8_t configuration = 1;
        assert(host_config.enum_filter_cb(&descriptor, &configuration) && configuration == 2);
        descriptor.idProduct = 0x8153; configuration = 1;
        assert(!host_config.enum_filter_cb(&descriptor, &configuration) && configuration == 1);
        descriptor.idProduct = 0x8152; descriptor.bNumConfigurations = 1;
        assert(!host_config.enum_filter_cb(&descriptor, &configuration));
        assert(!tater_usb_ethernet_configuration(0x0bda, 0x8152, 2, NULL));
        event(IOT_ETH_EVENT, IOT_ETH_EVENT_CONNECTED, &eth_handle);
        expect(TATER_NETWORK_WIFI, 1); assert(selected == &wifi); // no DHCP yet
        ip_event_got_ip_t wired_ip = { .esp_netif = wired, .ip_info.ip.addr = 20 };
        event(IP_EVENT, IP_EVENT_ETH_GOT_IP, &wired_ip);
        expect(TATER_NETWORK_USB_ETHERNET, 2); assert(selected == wired && mdns_enabled);
        event(IOT_ETH_EVENT, IOT_ETH_EVENT_CONNECTED, &eth_handle); // duplicate link event
        expect(TATER_NETWORK_USB_ETHERNET, 2);
        assert(strcmp(tater_usb_ethernet_status(), "connected") == 0);
        wifi_ip.ip_info.ip.addr = 11; event(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_ip);
        expect(TATER_NETWORK_USB_ETHERNET, 2); // backup DHCP renewal doesn't interrupt audio
        event(IP_EVENT, IP_EVENT_ETH_LOST_IP, &wired_ip);
        expect(TATER_NETWORK_WIFI, 3); assert(selected == &wifi && !mdns_enabled); // still has cable, no lease
        wired_ip.ip_info.ip.addr = 21; event(IP_EVENT, IP_EVENT_ETH_GOT_IP, &wired_ip);
        expect(TATER_NETWORK_USB_ETHERNET, 4);
        event(IOT_ETH_EVENT, IOT_ETH_EVENT_DISCONNECTED, &eth_handle);
        expect(TATER_NETWORK_WIFI, 5); assert(selected == &wifi && !mdns_enabled);
        event(IP_EVENT, IP_EVENT_ETH_GOT_IP, &wired_ip); // stale queued lease after unplug
        expect(TATER_NETWORK_WIFI, 5); assert(selected == &wifi && !mdns_enabled);
        event(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_ip);
        expect(TATER_NETWORK_NONE, 6);
        event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
        expect(TATER_NETWORK_NONE, 6);
        event(IOT_ETH_EVENT, IOT_ETH_EVENT_CONNECTED, &eth_handle);
        expect(TATER_NETWORK_NONE, 6); // reconnect must obtain an address again
        event(IP_EVENT, IP_EVENT_ETH_GOT_IP, &wired_ip);
        expect(TATER_NETWORK_USB_ETHERNET, 7);
        wired_ip.ip_info.ip.addr = 22;
        event(IP_EVENT, IP_EVENT_ETH_GOT_IP, &wired_ip);
        expect(TATER_NETWORK_USB_ETHERNET, 8); // active address changed
        event(IP_EVENT, IP_EVENT_ETH_GOT_IP, &wired_ip);
        expect(TATER_NETWORK_USB_ETHERNET, 8); // same lease doesn't restart sockets
        event(IP_EVENT, IP_EVENT_ETH_GOT_IP, &wifi_ip); // unrelated netif
        expect(TATER_NETWORK_USB_ETHERNET, 8);
        // Release the facade's permanent service allocations before leak checks.
        mdns_unregister_netif(wired);
        iot_eth_del_netif_glue(live_glue);
        esp_netif_destroy(wired);
        iot_eth_uninstall(eth_handle);
        usbh_cdc_driver_uninstall();
        usb_host_uninstall();
    }
    printf("USB Ethernet scenario passed: %s\n", scenario);
    return 0;
}
