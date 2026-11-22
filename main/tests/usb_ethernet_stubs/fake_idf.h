#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#define CONFIG_TATER_USB_ETHERNET 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_INTR_FLAG_LEVEL1 1
#define ESP_EVENT_ANY_ID -1
#define portMAX_DELAY UINT32_MAX
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) assert((x) == ESP_OK)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) 0U, 0U, 0U, (unsigned)(ip)->addr
#define USB_DEVICE_ID_MATCH_VID_PID 1
#define ESP_NETIF_INHERENT_DEFAULT_ETH() { .route_prio = 50 }
#define ESP_NETIF_NETSTACK_DEFAULT_ETH ((void *)1)
typedef int esp_err_t;
typedef int portMUX_TYPE;
typedef const char *esp_event_base_t;
extern const char *WIFI_EVENT, *IP_EVENT, *IOT_ETH_EVENT;
enum { WIFI_EVENT_STA_DISCONNECTED, IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP,
       IP_EVENT_ETH_GOT_IP, IP_EVENT_ETH_LOST_IP, IOT_ETH_EVENT_START,
       IOT_ETH_EVENT_STOP, IOT_ETH_EVENT_CONNECTED, IOT_ETH_EVENT_DISCONNECTED };
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct esp_netif { int route_prio; } esp_netif_t;
typedef struct { esp_netif_t *esp_netif; struct { esp_ip4_addr_t ip; } ip_info; } ip_event_got_ip_t;
typedef struct { const char *if_key, *if_desc; int route_prio; } esp_netif_inherent_config_t;
typedef struct { const esp_netif_inherent_config_t *base; const void *stack; } esp_netif_config_t;
typedef void (*event_handler_t)(void *, esp_event_base_t, int32_t, void *);
typedef void *esp_event_handler_instance_t;
typedef struct { uint16_t idVendor, idProduct; uint8_t bNumConfigurations; } usb_device_desc_t;
typedef struct { int intr_flags; bool (*enum_filter_cb)(const usb_device_desc_t *, uint8_t *); } usb_host_config_t;
typedef struct { size_t task_stack_size; unsigned task_priority; int task_coreid; bool skip_init_usb_host_driver; } usbh_cdc_driver_config_t;
typedef struct { int match_flags; uint16_t idVendor, idProduct; } usb_device_match_id_t;
typedef struct { const usb_device_match_id_t *match_id_list; } iot_usbh_ecm_config_t;
typedef struct { int unused; } iot_eth_driver_t;
typedef struct { iot_eth_driver_t *driver; } iot_eth_config_t;
typedef void *iot_eth_handle_t;
typedef void *iot_eth_netif_glue_handle_t;
esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, event_handler_t, void *);
esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t, event_handler_t, void *, esp_event_handler_instance_t *);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t, int32_t, esp_event_handler_instance_t);
esp_netif_t *esp_netif_new(const esp_netif_config_t *);
void esp_netif_destroy(esp_netif_t *);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
esp_err_t esp_netif_set_default_netif(esp_netif_t *);
esp_err_t esp_netif_attach(esp_netif_t *, void *);
bool usb_serial_jtag_is_connected(void);
const char *esp_err_to_name(esp_err_t);
void vTaskDelay(uint32_t);
int xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, unsigned, void *, int);
esp_err_t usb_host_install(const usb_host_config_t *);
esp_err_t usb_host_uninstall(void);
esp_err_t usb_host_lib_handle_events(uint32_t, uint32_t *);
esp_err_t usbh_cdc_driver_install(const usbh_cdc_driver_config_t *);
esp_err_t usbh_cdc_driver_uninstall(void);
esp_err_t iot_eth_new_usb_ecm(const iot_usbh_ecm_config_t *, iot_eth_driver_t **);
esp_err_t iot_eth_install(const iot_eth_config_t *, iot_eth_handle_t *);
esp_err_t iot_eth_uninstall(iot_eth_handle_t);
esp_err_t iot_eth_start(iot_eth_handle_t);
esp_err_t iot_eth_stop(iot_eth_handle_t);
iot_eth_netif_glue_handle_t iot_eth_new_netif_glue(iot_eth_handle_t);
esp_err_t iot_eth_del_netif_glue(iot_eth_netif_glue_handle_t);

typedef unsigned mdns_event_actions_t;
#define MDNS_EVENT_ENABLE_IP4 1U
#define MDNS_EVENT_ANNOUNCE_IP4 2U
#define MDNS_EVENT_DISABLE_IP4 4U
#define MDNS_EVENT_DISABLE_IP6 8U
esp_err_t mdns_init(void);
esp_err_t mdns_register_netif(esp_netif_t *);
esp_err_t mdns_unregister_netif(esp_netif_t *);
esp_err_t mdns_netif_action(esp_netif_t *, mdns_event_actions_t);
