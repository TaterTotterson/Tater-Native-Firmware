#include "usb_ethernet.h"

#include "sdkconfig.h"

#if CONFIG_TATER_USB_ETHERNET
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_event.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_eth.h"
#include "iot_eth_netif_glue.h"
#include "iot_usbh_cdc.h"
#include "iot_usbh_ecm.h"
#include "network_manager.h"
#include "mdns.h"
#include "usb/usb_host.h"

static const char *TAG = "tater_usb_eth";
static _Atomic(const char *) s_status = "not_started";
static iot_eth_handle_t s_eth;
static esp_netif_t *s_netif;

const char *tater_usb_ethernet_status(void)
{
    return atomic_load(&s_status);
}

static bool select_configuration(const usb_device_desc_t *desc, uint8_t *configuration)
{
    bool supported = tater_usb_ethernet_configuration(
        desc->idVendor, desc->idProduct, desc->bNumConfigurations, configuration);
    ESP_LOGI(TAG, "USB %04x:%04x configurations=%u %s", desc->idVendor,
             desc->idProduct, desc->bNumConfigurations, supported ? "using ECM config 2" : "unsupported");
    atomic_store(&s_status, supported ? "adapter_detected" : "unsupported_device");
    return supported;
}

static void ethernet_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IOT_ETH_EVENT) {
        if (!data || *(iot_eth_handle_t *)data != s_eth) return;
        if (id == IOT_ETH_EVENT_CONNECTED) {
            tater_network_state_t state;
            tater_network_snapshot(&state);
            tater_network_set_ethernet(true, state.ethernet_ip);
            atomic_store(&s_status, state.ethernet_ip ? "connected" : "waiting_for_dhcp");
        } else if (id == IOT_ETH_EVENT_DISCONNECTED || id == IOT_ETH_EVENT_STOP) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_netif_action(s_netif, MDNS_EVENT_DISABLE_IP4 | MDNS_EVENT_DISABLE_IP6));
            tater_network_set_ethernet(false, 0);
            atomic_store(&s_status, "waiting_for_link");
        }
    } else if (base == IP_EVENT && (id == IP_EVENT_ETH_GOT_IP || id == IP_EVENT_ETH_LOST_IP)) {
        ip_event_got_ip_t *event = data;
        if (!event || event->esp_netif != s_netif) return;
        if (id == IP_EVENT_ETH_GOT_IP) {
            tater_network_state_t state;
            tater_network_snapshot(&state);
            // A queued DHCP event must not resurrect a disconnected adapter.
            if (!state.ethernet_link) return;
            if (state.ethernet_ip && state.ethernet_ip != event->ip_info.ip.addr) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_netif_action(s_netif, MDNS_EVENT_DISABLE_IP4));
            }
            ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_netif_action(s_netif, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
            tater_network_set_ethernet(true, event->ip_info.ip.addr);
            atomic_store(&s_status, "connected");
            ESP_LOGI(TAG, "USB Ethernet ready: " IPSTR, IP2STR(&event->ip_info.ip));
        } else if (id == IP_EVENT_ETH_LOST_IP) {
            tater_network_state_t state;
            tater_network_snapshot(&state);
            ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_netif_action(s_netif, MDNS_EVENT_DISABLE_IP4));
            tater_network_set_ethernet(state.ethernet_link, 0);
            atomic_store(&s_status, state.ethernet_link ? "waiting_for_dhcp" : "waiting_for_link");
        }
    }
}

static void usb_events_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB event loop: %s", esp_err_to_name(err));
            tater_network_set_ethernet(false, 0);
            atomic_store(&s_status, "failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        /* CDC stays registered for hotplug; do not uninstall the host on unplug. */
    }
}

esp_err_t tater_usb_ethernet_start(void)
{
    /* One attempt per boot. Never take the shared PHY away from a connected PC. */
    if (strcmp(atomic_load(&s_status), "not_started") != 0) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i <= 50; ++i) {
        if (usb_serial_jtag_is_connected()) {
            atomic_store(&s_status, "console_preserved");
            ESP_LOGI(TAG, "PC detected; keeping USB Serial/JTAG for this boot");
            return ESP_OK;
        }
        if (i < 50) vTaskDelay(pdMS_TO_TICKS(10));
    }
    atomic_store(&s_status, "starting");
    bool host_installed = false, cdc_installed = false;
    bool eth_started = false, mdns_registered = false;
    iot_eth_driver_t *driver = NULL;
    iot_eth_netif_glue_handle_t glue = NULL;
    esp_event_handler_instance_t eth_events = NULL, ip_events = NULL;
    esp_err_t err;

    const usb_host_config_t host = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = select_configuration,
    };
    err = usb_host_install(&host);
    if (err != ESP_OK) goto fail;
    host_installed = true;
    const usbh_cdc_driver_config_t cdc = {
        .task_stack_size = 4096, .task_priority = 6, .task_coreid = 0,
        .skip_init_usb_host_driver = true,
    };
    err = usbh_cdc_driver_install(&cdc);
    if (err != ESP_OK) goto fail;
    cdc_installed = true;

    static const usb_device_match_id_t matches[] = {
        { .match_flags = USB_DEVICE_ID_MATCH_VID_PID, .idVendor = 0x0bda, .idProduct = 0x8152 },
        {0},
    };
    const iot_usbh_ecm_config_t ecm = { .match_id_list = matches };
    err = iot_eth_new_usb_ecm(&ecm, &driver);
    if (err != ESP_OK) goto fail;
    const iot_eth_config_t eth = { .driver = driver };
    err = iot_eth_install(&eth, &s_eth);
    if (err != ESP_OK) {
        /* Pinned ECM 0.4.0 unwinds init; its base is the first calloc member. */
        free(driver);
        driver = NULL;
        goto fail;
    }

    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.if_key = "TATER_USB_ETH";
    inherent.if_desc = "usb_ethernet";
    /* Wi-Fi STA is 100; the manager also excludes interfaces lacking an IP. */
    inherent.route_prio = 150;
    const esp_netif_config_t netif = {
        .base = &inherent, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&netif);
    if (!s_netif) { err = ESP_ERR_NO_MEM; goto fail; }
    glue = iot_eth_new_netif_glue(s_eth);
    if (!glue) { err = ESP_ERR_NO_MEM; goto fail; }
    err = esp_netif_attach(s_netif, glue);
    if (err != ESP_OK) goto fail;
    err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    err = mdns_register_netif(s_netif);
    if (err != ESP_OK) goto fail;
    mdns_registered = true;
    err = esp_event_handler_instance_register(IOT_ETH_EVENT, ESP_EVENT_ANY_ID,
                                             ethernet_event, NULL, &eth_events);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                             ethernet_event, NULL, &ip_events);
    if (err != ESP_OK) goto fail;
    err = iot_eth_start(s_eth);
    if (err != ESP_OK) goto fail;
    eth_started = true;
    atomic_store(&s_status, "waiting_for_adapter");
    /* Start enumeration only after every driver, glue and event handler is ready. */
    if (xTaskCreatePinnedToCore(usb_events_task, "tater_usb_host", 4096, NULL, 6, NULL, 0) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    return ESP_OK;

fail:
    atomic_store(&s_status, "failed");
    tater_network_set_ethernet(false, 0);
    if (eth_started) iot_eth_stop(s_eth);
    if (eth_events) esp_event_handler_instance_unregister(IOT_ETH_EVENT, ESP_EVENT_ANY_ID, eth_events);
    if (ip_events) esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_events);
    if (mdns_registered) mdns_unregister_netif(s_netif);
    if (glue) iot_eth_del_netif_glue(glue);
    if (s_netif) { esp_netif_destroy(s_netif); s_netif = NULL; }
    if (s_eth) { iot_eth_uninstall(s_eth); s_eth = NULL; }
    if (cdc_installed) usbh_cdc_driver_uninstall();
    if (host_installed) usb_host_uninstall();
    ESP_LOGE(TAG, "USB Ethernet startup failed: %s; Wi-Fi remains available", esp_err_to_name(err));
    return err;
}

#else
esp_err_t tater_usb_ethernet_start(void) { return ESP_OK; }
const char *tater_usb_ethernet_status(void) { return "disabled"; }
#endif
