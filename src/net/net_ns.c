/* net_ns.c — Network namespaces */

#define KERNEL_INTERNAL
#include "net_ns.h"
#include "net_internal.h"
#include "printf.h"
#include "string.h"
#include "e1000.h"

static struct net_ns net_namespaces[NET_NS_MAX];
static int current_ns_id = NET_NS_INIT;
static int net_ns_initialized = 0;

void net_ns_init(void) {
    memset(net_namespaces, 0, sizeof(net_namespaces));

    /* Initialize the default (init) namespace */
    struct net_ns *init_ns = &net_namespaces[NET_NS_INIT];
    init_ns->id = NET_NS_INIT;
    snprintf(init_ns->name, sizeof(init_ns->name), "init");
    init_ns->ip_addr = net_our_ip;
    init_ns->gateway = net_gateway_ip;
    init_ns->subnet_mask = net_subnet_mask;
    init_ns->dns_server = net_dns_server;
    if (e1000_is_present())
        e1000_get_mac(init_ns->mac);
    init_ns->in_use = 1;
    init_ns->num_ifaces = 1;
    init_ns->iface_ids[0] = 0;
    /* Copy routing table */
    init_ns->rt_num_entries = rt_num_entries;
    for (int i = 0; i < rt_num_entries; i++)
        init_ns->rt_table[i] = rt_table[i];

    current_ns_id = NET_NS_INIT;
    net_ns_initialized = 1;
    kprintf("[OK] Network namespaces initialized\\n");
}

struct net_ns *net_ns_create(const char *name) {
    if (!net_ns_initialized || !name) return NULL;

    for (int i = 0; i < NET_NS_MAX; i++) {
        if (!net_namespaces[i].in_use) {
            net_namespaces[i].id = i;
            snprintf(net_namespaces[i].name, sizeof(net_namespaces[i].name), "%s", name);
            net_namespaces[i].ip_addr = 0;
            net_namespaces[i].gateway = 0;
            net_namespaces[i].subnet_mask = 0;
            net_namespaces[i].dns_server = 0;
            memset(net_namespaces[i].mac, 0, 6);
            net_namespaces[i].in_use = 1;
            net_namespaces[i].num_ifaces = 0;
            net_namespaces[i].rt_num_entries = 0;
            return &net_namespaces[i];
        }
    }
    return NULL;
}

int net_ns_destroy(int ns_id) {
    if (!net_ns_initialized) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;
    if (ns_id == NET_NS_INIT) return -1;  /* can't destroy init_ns */
    if (!net_namespaces[ns_id].in_use) return -1;

    net_namespaces[ns_id].in_use = 0;
    return 0;
}

int net_ns_switch(int ns_id) {
    if (!net_ns_initialized) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;
    if (!net_namespaces[ns_id].in_use) return -1;

    current_ns_id = ns_id;
    return 0;
}

struct net_ns *net_ns_get_current(void) {
    if (!net_ns_initialized) return NULL;
    return &net_namespaces[current_ns_id];
}

struct net_ns *net_ns_get_init(void) {
    if (!net_ns_initialized) return NULL;
    return &net_namespaces[NET_NS_INIT];
}

int net_ns_get_id(void) {
    return current_ns_id;
}
