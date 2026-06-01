#ifndef NET_NS_H
#define NET_NS_H

#include "types.h"
#include "net_internal.h"

/* Maximum number of network namespaces */
#define NET_NS_MAX 4

/* Default namespace index */
#define NET_NS_INIT 0

/* Network namespace */
struct net_ns {
    int      id;
    char     name[32];
    uint32_t ip_addr;
    uint32_t gateway;
    uint32_t subnet_mask;
    uint8_t  mac[6];
    uint32_t dns_server;
    int      in_use;
    /* Device list — simplified: just track the interface index */
    int      num_ifaces;
    int      iface_ids[16];
    /* Routing table per namespace (uses struct rt_entry from net_internal.h) */
    struct rt_entry rt_table[16];
    int      rt_num_entries;
};

/* ── API ────────────────────────────────────────────────────────── */

struct net_ns *net_ns_create(const char *name);
int  net_ns_destroy(int ns_id);
int  net_ns_switch(int ns_id);
struct net_ns *net_ns_get_current(void);
struct net_ns *net_ns_get_init(void);
int  net_ns_get_id(void);

void net_ns_init(void);

#endif /* NET_NS_H */
