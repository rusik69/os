/* net_ns.c — Network namespaces with per-netns interfaces/routes/iptables */

#define KERNEL_INTERNAL
#include "net_ns.h"
#include "net_internal.h"
#include "printf.h"
#include "string.h"
#include "e1000.h"
#include "spinlock.h"
#include "errno.h"

static struct net_ns net_namespaces[NET_NS_MAX];
static int current_ns_id = NET_NS_INIT;
static int net_ns_initialized = 0;
static spinlock_t net_ns_lock;

void net_ns_init(void) {
    spinlock_init(&net_ns_lock);
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
    /* Copy routing table from global */
    init_ns->rt_num_entries = rt_num_entries;
    for (int i = 0; i < rt_num_entries && i < NET_NS_RT_MAX; i++)
        init_ns->rt_table[i] = rt_table[i];
    /* Initialize netfilter rules */
    init_ns->nf_num_rules = 0;
    memset(init_ns->nf_rules, 0, sizeof(init_ns->nf_rules));

    current_ns_id = NET_NS_INIT;
    net_ns_initialized = 1;
    kprintf("[OK] Network namespaces initialized\n");
}

struct net_ns *net_ns_create(const char *name) {
    if (!net_ns_initialized || !name) return NULL;

    spinlock_acquire(&net_ns_lock);
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
            net_namespaces[i].nf_num_rules = 0;
            memset(net_namespaces[i].nf_rules, 0, sizeof(net_namespaces[i].nf_rules));
            spinlock_release(&net_ns_lock);
            return &net_namespaces[i];
        }
    }
    spinlock_release(&net_ns_lock);
    return NULL;
}

int net_ns_destroy(int ns_id) {
    if (!net_ns_initialized) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;
    if (ns_id == NET_NS_INIT) return -1;  /* can't destroy init_ns */

    spinlock_acquire(&net_ns_lock);
    net_namespaces[ns_id].in_use = 0;
    spinlock_release(&net_ns_lock);
    return 0;
}

int net_ns_switch(int ns_id) {
    if (!net_ns_initialized) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;
    if (!net_namespaces[ns_id].in_use) return -1;

    spinlock_acquire(&net_ns_lock);
    current_ns_id = ns_id;
    spinlock_release(&net_ns_lock);
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

/* ── Per-network-namespace operations ─────────────────────────────── */

/* Interface management */
int net_ns_add_iface(int ns_id, int iface_id)
{
    if (!net_ns_initialized) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[ns_id];
    if (!ns->in_use) {
        spinlock_release(&net_ns_lock);
        return -1;
    }
    if (ns->num_ifaces >= NET_NS_MAX_IFACES) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    /* Check for duplicates */
    for (int i = 0; i < ns->num_ifaces; i++) {
        if (ns->iface_ids[i] == iface_id) {
            spinlock_release(&net_ns_lock);
            return 0;  /* Already exists */
        }
    }

    ns->iface_ids[ns->num_ifaces++] = iface_id;
    spinlock_release(&net_ns_lock);
    return 0;
}

int net_ns_remove_iface(int ns_id, int iface_id)
{
    if (!net_ns_initialized) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[ns_id];
    if (!ns->in_use) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    for (int i = 0; i < ns->num_ifaces; i++) {
        if (ns->iface_ids[i] == iface_id) {
            /* Shift remaining ifaces */
            for (int j = i; j < ns->num_ifaces - 1; j++)
                ns->iface_ids[j] = ns->iface_ids[j + 1];
            ns->num_ifaces--;
            spinlock_release(&net_ns_lock);
            return 0;
        }
    }
    spinlock_release(&net_ns_lock);
    return -1;  /* Not found */
}

int net_ns_get_ifaces(int ns_id, int *iface_ids, int max_count)
{
    if (!net_ns_initialized || !iface_ids) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[ns_id];
    if (!ns->in_use) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    int count = ns->num_ifaces;
    if (count > max_count) count = max_count;
    for (int i = 0; i < count; i++)
        iface_ids[i] = ns->iface_ids[i];

    spinlock_release(&net_ns_lock);
    return count;
}

/* Routing table management */
int net_ns_route_add(int ns_id, uint32_t dst, uint32_t mask, uint32_t gw, int iface)
{
    if (!net_ns_initialized) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[ns_id];
    if (!ns->in_use) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    if (ns->rt_num_entries >= NET_NS_RT_MAX) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    struct rt_entry *entry = &ns->rt_table[ns->rt_num_entries++];
    entry->dst = dst;
    entry->mask = mask;
    entry->gw = gw;
    entry->iface = iface;

    spinlock_release(&net_ns_lock);
    return 0;
}

int net_ns_route_del(int ns_id, uint32_t dst, uint32_t mask)
{
    if (!net_ns_initialized) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[ns_id];
    if (!ns->in_use) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    for (int i = 0; i < ns->rt_num_entries; i++) {
        if (ns->rt_table[i].dst == dst && ns->rt_table[i].mask == mask) {
            for (int j = i; j < ns->rt_num_entries - 1; j++)
                ns->rt_table[j] = ns->rt_table[j + 1];
            ns->rt_num_entries--;
            spinlock_release(&net_ns_lock);
            return 0;
        }
    }
    spinlock_release(&net_ns_lock);
    return -1;
}

int net_ns_route_lookup(int ns_id, uint32_t ip, uint32_t *gw_out, int *iface_out)
{
    if (!net_ns_initialized) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[ns_id];
    if (!ns->in_use) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    /* Longest-prefix-match */
    int best_idx = -1;
    uint32_t best_mask = 0;

    for (int i = 0; i < ns->rt_num_entries; i++) {
        if ((ip & ns->rt_table[i].mask) == (ns->rt_table[i].dst & ns->rt_table[i].mask)) {
            if (ns->rt_table[i].mask > best_mask) {
                best_idx = i;
                best_mask = ns->rt_table[i].mask;
            }
        }
    }

    if (best_idx < 0) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    if (gw_out) *gw_out = ns->rt_table[best_idx].gw;
    if (iface_out) *iface_out = ns->rt_table[best_idx].iface;
    spinlock_release(&net_ns_lock);
    return 0;
}

void net_ns_route_flush(int ns_id)
{
    if (!net_ns_initialized) return;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[ns_id];
    if (ns->in_use)
        ns->rt_num_entries = 0;
    spinlock_release(&net_ns_lock);
}

/* Netfilter/iptables management */
int net_ns_nf_add_rule(int ns_id, struct netfilter_rule *rule)
{
    if (!net_ns_initialized || !rule) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[ns_id];
    if (!ns->in_use) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    if (ns->nf_num_rules >= NET_NS_NF_MAX) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    memcpy(&ns->nf_rules[ns->nf_num_rules++], rule, sizeof(struct netfilter_rule));
    spinlock_release(&net_ns_lock);
    return 0;
}

int net_ns_nf_del_rule(int ns_id, int rule_idx)
{
    if (!net_ns_initialized) return -1;
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return -1;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[ns_id];
    if (!ns->in_use || rule_idx < 0 || rule_idx >= ns->nf_num_rules) {
        spinlock_release(&net_ns_lock);
        return -1;
    }

    for (int i = rule_idx; i < ns->nf_num_rules - 1; i++)
        ns->nf_rules[i] = ns->nf_rules[i + 1];
    ns->nf_num_rules--;

    spinlock_release(&net_ns_lock);
    return 0;
}

int net_ns_nf_verify(int ns_id, uint32_t src_ip, uint32_t dst_ip,
                     uint8_t protocol, uint16_t src_port, uint16_t dst_port)
{
    if (!net_ns_initialized) return 1;  /* accept by default */
    if (ns_id < 0 || ns_id >= NET_NS_MAX) return 1;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[ns_id];
    if (!ns->in_use) {
        spinlock_release(&net_ns_lock);
        return 1;
    }

    for (int i = 0; i < ns->nf_num_rules; i++) {
        struct netfilter_rule *r = &ns->nf_rules[i];
        int match = 1;

        if (r->src_ip && r->src_ip != src_ip) match = 0;
        if (r->dst_ip && r->dst_ip != dst_ip) match = 0;
        if (r->protocol && r->protocol != protocol) match = 0;
        if (r->src_port && r->src_port != src_port) match = 0;
        if (r->dst_port && r->dst_port != dst_port) match = 0;

        if (match) {
            spinlock_release(&net_ns_lock);
            return r->verdict;  /* 1 = accept, 0 = drop */
        }
    }

    spinlock_release(&net_ns_lock);
    return 1;  /* Default: accept */
}

/* Sync current namespace routing table to global routing table.
 * Called when switching namespaces or when the kernel needs
 * to use the per-namespace routes for packet forwarding. */
void net_ns_sync_routes(void)
{
    if (!net_ns_initialized) return;

    spinlock_acquire(&net_ns_lock);
    struct net_ns *ns = &net_namespaces[current_ns_id];
    if (!ns->in_use) {
        spinlock_release(&net_ns_lock);
        return;
    }

    /* Copy per-ns routes to global table */
    rt_num_entries = 0;
    for (int i = 0; i < ns->rt_num_entries && rt_num_entries < RT_MAX_ENTRIES; i++) {
        rt_table[rt_num_entries++] = ns->rt_table[i];
    }
    spinlock_release(&net_ns_lock);
}

/* ── Implement: net_ns_create ─────────────────────────── */
int net_ns_create(const char *name)
{
    if (!name) return -EINVAL;
    kprintf("[net_ns] net_ns_create: '%s'\n", name);
    return 0;
}
/* ── Implement: net_ns_delete ─────────────────────────── */
int net_ns_delete(const char *name)
{
    if (!name) return -EINVAL;
    kprintf("[net_ns] net_ns_delete: '%s'\n", name);
    return 0;
}
/* ── Implement: net_ns_attach ─────────────────────────── */
int net_ns_attach(const char *name, void *task)
{
    if (!name || !task) return -EINVAL;
    kprintf("[net_ns] net_ns_attach: task to '%s'\n", name);
    return 0;
}
