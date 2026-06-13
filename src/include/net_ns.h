#ifndef NET_NS_H
#define NET_NS_H

#include "types.h"
#include "net_internal.h"

/* Maximum number of network namespaces */
#define NET_NS_MAX 4

/* Default namespace index */
#define NET_NS_INIT 0

/* Maximum interfaces per namespace */
#define NET_NS_MAX_IFACES 16

/* Maximum routing entries per namespace */
#define NET_NS_RT_MAX 16

/* Maximum netfilter rules per namespace */
#define NET_NS_NF_MAX 32

/* Netfilter rule */
struct netfilter_rule {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  protocol;       /* IP protocol number (0 = any) */
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  verdict;        /* 1 = ACCEPT, 0 = DROP */
    char     name[32];       /* Rule name for identification */
};

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

    /* Device list */
    int      num_ifaces;
    int      iface_ids[NET_NS_MAX_IFACES];

    /* Per-namespace routing table */
    struct rt_entry rt_table[NET_NS_RT_MAX];
    int      rt_num_entries;

    /* Per-namespace netfilter/iptables rules */
    struct netfilter_rule nf_rules[NET_NS_NF_MAX];
    int      nf_num_rules;
};

/* ── Core namespace operations ────────────────────────────────────── */

struct net_ns *net_ns_create(const char *name);
int  net_ns_destroy(int ns_id);
int  net_ns_switch(int ns_id);
struct net_ns *net_ns_get_current(void);
struct net_ns *net_ns_get_init(void);
int  net_ns_get_id(void);
void net_ns_init(void);

/* ── Per-namespace interface management ──────────────────────────── */

int net_ns_add_iface(int ns_id, int iface_id);
int net_ns_remove_iface(int ns_id, int iface_id);
int net_ns_get_ifaces(int ns_id, int *iface_ids, int max_count);

/* ── Per-namespace routing table ─────────────────────────────────── */

int  net_ns_route_add(int ns_id, uint32_t dst, uint32_t mask, uint32_t gw, int iface);
int  net_ns_route_del(int ns_id, uint32_t dst, uint32_t mask);
int  net_ns_route_lookup(int ns_id, uint32_t ip, uint32_t *gw_out, int *iface_out);
void net_ns_route_flush(int ns_id);

/* ── Per-namespace netfilter/iptables ─────────────────────────────── */

int net_ns_nf_add_rule(int ns_id, struct netfilter_rule *rule);
int net_ns_nf_del_rule(int ns_id, int rule_idx);
int net_ns_nf_verify(int ns_id, uint32_t src_ip, uint32_t dst_ip,
                     uint8_t protocol, uint16_t src_port, uint16_t dst_port);

/* ── Utility ──────────────────────────────────────────────────────── */

/* Sync current namespace's routing table to the global routing table */
void net_ns_sync_routes(void);

#endif /* NET_NS_H */
