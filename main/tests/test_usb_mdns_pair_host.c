#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MDNS_MAX_INTERFACES 3
typedef unsigned mdns_if_t;
enum { MDNS_IF_STA, MDNS_IF_AP, MDNS_IF_ETH };
typedef struct { const char *key; uint32_t ip; } esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip; } esp_netif_ip_info_t;
typedef struct {
    bool predefined;
    esp_netif_t *netif;
    unsigned predef_if;
    mdns_if_t duplicate;
} mdns_interfaces_t;
static mdns_interfaces_t s_esp_netifs[MDNS_MAX_INTERFACES];
static unsigned duplicate_calls;
static const char *esp_netif_get_ifkey(esp_netif_t *netif) { return netif->key; }
static esp_netif_t *mdns_priv_get_esp_netif(mdns_if_t i) { return s_esp_netifs[i].netif; }
static int esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    if (!netif || !netif->ip) return -1;
    info->ip.addr = netif->ip;
    return 0;
}
static void mdns_priv_pcb_set_duplicate(mdns_if_t i)
{
    assert(i == 0);
    ++duplicate_calls;
}

/* ACTUAL_PAIR_LOOKUP */
/* ACTUAL_COLLISION_CHECK */

int main(void)
{
    esp_netif_t wifi = { "WIFI_STA_DEF", 0x1314040a };
    esp_netif_t usb = { "TATER_USB_ETH", 0x9f14040a };
    for (unsigned i = 0; i < MDNS_MAX_INTERFACES; ++i)
        s_esp_netifs[i].duplicate = MDNS_MAX_INTERFACES;
    s_esp_netifs[0] = (mdns_interfaces_t){true, &wifi, MDNS_IF_STA, MDNS_MAX_INTERFACES};
    s_esp_netifs[1] = (mdns_interfaces_t){true, NULL, MDNS_IF_AP, MDNS_MAX_INTERFACES};
    s_esp_netifs[2] = (mdns_interfaces_t){false, &usb, 0, MDNS_MAX_INTERFACES};
    esp_ip4_addr_t incoming = {usb.ip};
#if CONFIG_TATER_USB_ETHERNET
    assert(mdns_priv_netif_get_other_interface(0) == 2);
    assert(mdns_priv_netif_get_other_interface(2) == 0);
    // Reproduce the two real addresses: an own wired probe received on Wi-Fi
    // must coalesce interfaces rather than lose and rename the hostname.
    assert(check_a_collision(&incoming, 0) == 2);
    assert(duplicate_calls == 1);
    usb.ip = 0;
    assert(check_a_collision(&incoming, 0) == 1);
    usb.ip = incoming.addr;
#else
    assert(mdns_priv_netif_get_other_interface(0) == MDNS_MAX_INTERFACES);
    assert(check_a_collision(&incoming, 0) == 1);
    assert(duplicate_calls == 0);
#endif
    assert(mdns_priv_netif_get_other_interface(1) == MDNS_MAX_INTERFACES);
    assert(mdns_priv_netif_get_other_interface(MDNS_MAX_INTERFACES) == MDNS_MAX_INTERFACES);
    incoming.addr = 0xf014040a;
    assert(check_a_collision(&incoming, 0) == 1); // a real conflicting device
    incoming.addr = wifi.ip;
    assert(check_a_collision(&incoming, 0) == 0);
    s_esp_netifs[2].netif = NULL; // unregister/unwind
    assert(mdns_priv_netif_get_other_interface(0) == MDNS_MAX_INTERFACES);
    s_esp_netifs[2].netif = &usb;
    usb.key = "OTHER_CUSTOM_NETIF";
    assert(mdns_priv_netif_get_other_interface(0) == MDNS_MAX_INTERFACES);
    // Preserve the production library's standard Ethernet pairing.
    s_esp_netifs[0].duplicate = 2;
    s_esp_netifs[2] = (mdns_interfaces_t){true, &usb, MDNS_IF_ETH, 0};
    assert(mdns_priv_netif_get_other_interface(0) == 2);
    assert(mdns_priv_netif_get_other_interface(2) == 0);
    puts("mDNS custom ECM/STA collision tests passed");
    return 0;
}
