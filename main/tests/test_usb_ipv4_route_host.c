#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define LWIP_SINGLE_NETIF 0
#define LWIP_MULTICAST_TX_OPTIONS 1
#define LWIP_NETIF_LOOPBACK 1
#define LWIP_HAVE_LOOPIF 0
#define LWIP_HOOK_IP4_ROUTE_SRC ip4_route_src_hook
#define NETIF_FLAG_BROADCAST 1
#define LWIP_ASSERT_CORE_LOCKED() ((void)0)
#define LWIP_UNUSED_ARG(x) ((void)(x))
#define LWIP_DEBUGF(a, b) ((void)0)
#define IP_STATS_INC(x) ((void)0)
#define MIB2_STATS_INC(x) ((void)0)
typedef struct { uint32_t addr; } ip4_addr_t;
struct netif {
    struct netif *next;
    bool up, link;
    unsigned flags;
    ip4_addr_t ip, mask, gw;
};
static struct netif *netif_list, *netif_default, *ip4_default_multicast_netif;
#define NETIF_FOREACH(n) for ((n) = netif_list; (n); (n) = (n)->next)
#define netif_is_up(n) ((n)->up)
#define netif_is_link_up(n) ((n)->link)
#define netif_ip4_addr(n) (&(n)->ip)
#define netif_ip4_netmask(n) (&(n)->mask)
#define netif_ip4_gw(n) (&(n)->gw)
#define ip4_addr_isany_val(a) ((a).addr == 0)
#define ip4_addr_isany(a) ((a)->addr == 0)
#define ip4_addr_eq(a,b) ((a)->addr == (b)->addr)
#define ip4_addr_cmp(a,b) ip4_addr_eq(a,b)
#define ip4_addr_net_eq(a,b,m) (((a)->addr & (m)->addr) == ((b)->addr & (m)->addr))
#define ip4_addr_isloopback(a) (((a)->addr >> 24) == 127)
#define ip4_addr_ismulticast(a) (((a)->addr & 0xf0000000) == 0xe0000000)
struct netif *ip4_route(const ip4_addr_t *dest);
/* ACTUAL_SOURCE_HOOK */
/* ACTUAL_SOURCE_ROUTE */
/* ACTUAL_IPV4_ROUTE */

int main(void)
{
    // Reproduce hardware order: fallback Wi-Fi comes before preferred USB.
    struct netif ap = {NULL, true, true, 1, {0x0a050001}, {0xffffff00}, {0}};
    struct netif usb = {&ap, true, true, 1, {0x0a04149f}, {0xffffff00}, {0x0a041401}};
    struct netif wifi = {&usb, true, true, 1, {0x0a041413}, {0xffffff00}, {0x0a041401}};
    netif_list = &wifi;
    netif_default = &usb;
    ip4_addr_t server = {0x0a0414d2}, any = {0}, internet = {0x08080808};
#if CONFIG_TATER_USB_ETHERNET
    assert(ip4_route(&server) == &usb);
    assert(ip4_route_src(&any, &server) == &usb);
#else
    assert(ip4_route(&server) == &wifi); // original bug, ordinary builds preserved
    assert(ip4_route_src(&any, &server) == &wifi);
#endif
    // Explicit source addresses must retain their own interface.
    assert(ip4_route_src(&wifi.ip, &server) == &wifi);
    assert(ip4_route_src(&usb.ip, &server) == &usb);
    assert(ip4_route(&internet) == &usb);
    usb.link = false;
    assert(ip4_route(&server) == &wifi);
    usb.link = true;
    usb.up = false;
    assert(ip4_route(&server) == &wifi);
    usb.up = true;
    uint32_t saved = usb.ip.addr;
    usb.ip.addr = 0; // linked but no DHCP address
    assert(ip4_route(&server) == &wifi);
    usb.ip.addr = saved;
    netif_default = &wifi; // fallback transition
    assert(ip4_route(&server) == &wifi);
    netif_default = &usb;
    ip4_addr_t setup_peer = {0x0a050002}, multicast = {0xe00000fb}, loop = {0x7f000001};
    assert(ip4_route(&setup_peer) == &ap);
    ip4_default_multicast_netif = &wifi;
    assert(ip4_route(&multicast) == &wifi);
    assert(ip4_route(&loop) == &usb);
    usb.ip.addr = 0x0a060002; // Ethernet on a different subnet
    assert(ip4_route(&server) == &wifi);
    puts("IPv4 shared-subnet preference and source routing tests passed");
    return 0;
}
