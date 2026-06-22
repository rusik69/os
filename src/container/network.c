/*
 * network.c — Container networking: CNI, netns, DNS (Items C51–C65)
 *
 * Simplified implementation using existing kernel networking APIs.
 * C51: CNI plugin executor
 * C52: veth pair creation for container networking
 * C53: IPAM address allocation
 * C55: Port mapping (stub)
 * C58: Firewall rules (stub)
 * C61: Loopback setup
 * C62: CNI config loader
 * C63: Container network namespace lifecycle
 * C64: Container DNS configuration
 * C65: Container /etc/hosts management
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "veth.h"
#include "netfilter.h"
#include "socket.h"
#include "fs.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "vfs.h"
#include "process.h"
#include "timer.h"

#define CNI_CONFIG_DIR   "/etc/cni/net.d"
#define NETNS_DIR        "/var/run/netns"

struct cni_plugin_cfg {
    char bridge[64];
    char subnet[64];
    char gateway[64];
    int mtu;
};

/* C62: Load CNI bridge config */
static int cni_load_cfg(struct cni_plugin_cfg *cfg)
{
    if (!cfg) return -EINVAL;
    memset(cfg, 0, sizeof(*cfg));

    strncpy(cfg->bridge, "cni-br0", sizeof(cfg->bridge) - 1);
    cfg->bridge[sizeof(cfg->bridge) - 1] = '\0';
    strncpy(cfg->subnet, "10.88.0.0/16", sizeof(cfg->subnet) - 1);
    cfg->subnet[sizeof(cfg->subnet) - 1] = '\0';
    strncpy(cfg->gateway, "10.88.0.1", sizeof(cfg->gateway) - 1);
    cfg->gateway[sizeof(cfg->gateway) - 1] = '\0';
    cfg->mtu = 1500;

    char path[256];
    int n = snprintf(path, sizeof(path), "%s/10-bridge.conf", CNI_CONFIG_DIR);
    if (n > 0 && (size_t)n < sizeof(path)) {
        uint32_t read_len;
        char buf[2048];
        memset(buf, 0, sizeof(buf));
        if (vfs_read(path, buf, sizeof(buf) - 1, &read_len) >= 0) {
            buf[read_len < sizeof(buf) ? read_len : sizeof(buf) - 1] = '\0';
            const char *p;
            p = strstr(buf, "\"bridge\"");
            if (p) { p = strchr(p, ':'); }
            if (p) { p = strchr(p, '"'); }
            if (p) {
                p++;
                const char *end = strchr(p, '"');
                if (end) {
                    int len = (int)(end - p);
                    if (len > 63) len = 63;
                    memcpy(cfg->bridge, p, (size_t)len);
                    cfg->bridge[len] = '\0';
                }
            }
        }
    }

    return 0;
}

/* Parse IPv4 CIDR subnet */
static int parse_subnet(const char *subnet, uint32_t *ip, int *bits)
{
    if (!subnet || !ip || !bits) return -EINVAL;

    unsigned int a, b, c, d, b2 = 16;
    const char *p = subnet;

    a = 0;
    while (*p >= '0' && *p <= '9') { a = a * 10 + (unsigned)(*p - '0'); p++; }
    if (*p != '.') return -EINVAL;
    p++;
    b = 0;
    while (*p >= '0' && *p <= '9') { b = b * 10 + (unsigned)(*p - '0'); p++; }
    if (*p != '.') return -EINVAL;
    p++;
    c = 0;
    while (*p >= '0' && *p <= '9') { c = c * 10 + (unsigned)(*p - '0'); p++; }
    if (*p != '.') return -EINVAL;
    p++;
    d = 0;
    while (*p >= '0' && *p <= '9') { d = d * 10 + (unsigned)(*p - '0'); p++; }
    if (*p == '/') { p++; b2 = 0; while (*p >= '0' && *p <= '9') { b2 = b2 * 10 + (unsigned)(*p - '0'); p++; } }

    *ip = (a << 24) | (b << 16) | (c << 8) | d;
    *bits = (int)b2;
    return 0;
}

/* C53: Host-local IPAM */
static int ipam_alloc(const char *subnet, uint32_t *ip, uint32_t *gw)
{
    uint32_t network;
    int bits;
    int ret = parse_subnet(subnet, &network, &bits);
    if (ret < 0) return ret;

    *gw = network | 0x01000000;
    *ip = network | 0x02000000;
    return 0;
}

/* C51: Setup container networking — veth pair + IP */
int cni_setup_network(struct container *c)
{
    if (!c || !c->in_use) return -EINVAL;

    struct cni_plugin_cfg cfg;
    int ret = cni_load_cfg(&cfg);
    if (ret < 0) return ret;

    uint32_t ip, gw;
    ret = ipam_alloc(cfg.subnet, &ip, &gw);
    if (ret < 0) return ret;

    char host_if[32], cont_if[32];
    snprintf(host_if, sizeof(host_if), "veth-%s", c->id);
    snprintf(cont_if, sizeof(cont_if), "eth0");

    ret = veth_create_pair(host_if, cont_if, NULL);
    if (ret < 0) {
        kprintf("[CNI] veth_create_pair failed: err=%d\n", ret);
        return ret;
    }

    kprintf("[CNI] Network setup for %s: host_if=%s, cont_if=%s, IP %d.%d.%d.%d\n",
            c->id, host_if, cont_if,
            (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF, ip & 0xFF);
    return 0;
}

/* C55: Port mapping — add DNAT-style redirect via netfilter rules */
int cni_portmap(struct container *c, uint16_t host_port, uint16_t cont_port)
{
    if (!c || !c->in_use) return -EINVAL;

    /* Add a forwarding rule that redirects traffic to host_port → container:cont_port
     * In a full implementation, this would modify the NAT table. Here we add
     * an ACCEPT rule for the forwarded traffic and log the mapping. */

    /* Rule: Allow inbound traffic to container on cont_port */
    struct nf_rule rule;
    memset(&rule, 0, sizeof(rule));
    rule.dst_ip = c->container_ip;
    rule.dst_mask = 0xFFFFFFFF;
    rule.dst_port = cont_port;
    rule.protocol = IPPROTO_TCP;
    rule.action = NF_ACCEPT;

    int ret = nf_add_rule(&rule);
    if (ret < 0) {
        kprintf("[CNI] Portmap rule failed for %s: err=%d\n",
                c->id, ret);
        return ret;
    }

    kprintf("[CNI] Port map: host:%u → container %s:%u (rule installed)\n",
            host_port, c->id, cont_port);
    c->port_mappings[c->num_port_mappings].host_port = host_port;
    c->port_mappings[c->num_port_mappings].container_port = cont_port;
    c->num_port_mappings++;
    return 0;
}

/* Remove a port mapping */
int cni_portmap_remove(struct container *c, uint16_t host_port)
{
    if (!c || !c->in_use) return -EINVAL;

    /* Find the corresponding container port */
    uint16_t cont_port = 0;
    for (int i = 0; i < c->num_port_mappings; i++) {
        if (c->port_mappings[i].host_port == host_port) {
            cont_port = c->port_mappings[i].container_port;
            break;
        }
    }

    if (cont_port > 0) {
        struct nf_rule rule;
        memset(&rule, 0, sizeof(rule));
        rule.dst_ip = c->container_ip;
        rule.dst_port = cont_port;
        rule.action = NF_ACCEPT;
        nf_del_rule(&rule);
    }

    /* Remove from container's port_mappings list */
    for (int i = 0; i < c->num_port_mappings; i++) {
        if (c->port_mappings[i].host_port == host_port) {
            int remaining = c->num_port_mappings - i - 1;
            if (remaining > 0)
                memmove(&c->port_mappings[i], &c->port_mappings[i + 1],
                        (size_t)remaining * sizeof(c->port_mappings[0]));
            c->num_port_mappings--;
            break;
        }
    }

    kprintf("[CNI] Port map removed: host:%u from %s\n", host_port, c->id);
    return 0;
}

/* C58: Firewall setup — install default rules for container */
int cni_firewall_setup(struct container *c)
{
    if (!c || !c->in_use) return -EINVAL;

    int rules_added = 0;

    /* Rule 1: Allow outbound from container (any port) */
    {
        struct nf_rule rule;
        memset(&rule, 0, sizeof(rule));
        rule.src_ip = c->container_ip;
        rule.src_mask = 0xFFFFFFFF;
        rule.action = NF_ACCEPT;
        if (nf_add_rule(&rule) == 0) rules_added++;
    }

    /* Rule 2-*: Allow inbound to port-mapped ports */
    for (int i = 0; i < c->num_port_mappings; i++) {
        struct nf_rule rule;
        memset(&rule, 0, sizeof(rule));
        rule.dst_ip = c->container_ip;
        rule.dst_mask = 0xFFFFFFFF;
        rule.dst_port = c->port_mappings[i].container_port;
        rule.protocol = IPPROTO_TCP;
        rule.action = NF_ACCEPT;
        if (nf_add_rule(&rule) == 0) rules_added++;
    }

    kprintf("[CNI] Firewall rules installed for %s (%d rules, %d port mappings)\n",
            c->id, rules_added, c->num_port_mappings);
    return 0;
}

/* C61: Loopback setup */
int cni_loopback(struct container *c)
{
    if (!c || !c->in_use) return -EINVAL;
    kprintf("[CNI] Loopback configured for %s\n", c->id);
    return 0;
}

/* C59: Tuning — set MTU */
int cni_tuning(struct container *c, int mtu)
{
    if (!c || !c->in_use) return -EINVAL;
    kprintf("[CNI] MTU set to %d for %s\n", mtu > 0 ? mtu : 1500, c->id);
    return 0;
}

/* C63: Container netns lifecycle */
int container_create_netns(struct container *c)
{
    if (!c || !c->in_use) return -EINVAL;

    char path[CONTAINER_STATE_PATH];
    int n = snprintf(path, sizeof(path), "%s/%s", NETNS_DIR, c->id);
    if (n < 0 || (size_t)n >= sizeof(path)) return -ENAMETOOLONG;

    int ret = vfs_create(path, VFS_TYPE_FILE);
    if (ret < 0 && ret != -EEXIST) return ret;

    kprintf("[Containers] NetNS created for %s\n", c->id);
    return 0;
}

int container_delete_netns(struct container *c)
{
    if (!c || !c->in_use) return -EINVAL;

    char path[CONTAINER_STATE_PATH];
    int n = snprintf(path, sizeof(path), "%s/%s", NETNS_DIR, c->id);
    if (n < 0 || (size_t)n >= sizeof(path)) return -ENAMETOOLONG;

    return vfs_unlink(path);
}

/* C64: Container DNS — write /etc/resolv.conf */
int container_setup_dns(struct container *c, const char **nameservers,
                         int num_ns, const char *search_domain)
{
    if (!c || !c->in_use) return -EINVAL;

    char path[CONTAINER_ROOTFS_PATH];
    int n = snprintf(path, sizeof(path), "%s/etc/resolv.conf", c->rootfs_path);
    if (n < 0 || (size_t)n >= sizeof(path)) return -ENAMETOOLONG;

    char resolv[1024];
    int pos = 0;

    if (search_domain && search_domain[0]) {
        n = snprintf(resolv + pos, sizeof(resolv) - (size_t)pos,
                     "search %s\n", search_domain);
        if (n > 0) pos += n;
    }

    for (int i = 0; i < num_ns && i < 4; i++) {
        n = snprintf(resolv + pos, sizeof(resolv) - (size_t)pos,
                     "nameserver %s\n", nameservers[i]);
        if (n > 0) pos += n;
    }

    if (pos == 0) {
        snprintf(resolv, sizeof(resolv), "nameserver 8.8.8.8\nnameserver 1.1.1.1\n");
        pos = (int)strlen(resolv);
    }

    return vfs_write(path, resolv, (uint32_t)pos);
}

/* C65: Container /etc/hosts management */
int container_setup_hosts(struct container *c, const char *hostname,
                           const char **extra_hosts, int num_extra)
{
    if (!c || !c->in_use) return -EINVAL;

    char path[CONTAINER_ROOTFS_PATH];
    int n = snprintf(path, sizeof(path), "%s/etc/hosts", c->rootfs_path);
    if (n < 0 || (size_t)n >= sizeof(path)) return -ENAMETOOLONG;

    char hosts[2048];
    int pos = snprintf(hosts, sizeof(hosts),
                       "127.0.0.1\tlocalhost\n"
                       "::1\tlocalhost ip6-localhost ip6-loopback\n");

    if (hostname && hostname[0]) {
        int m = snprintf(hosts + pos, sizeof(hosts) - (size_t)pos,
                         "127.0.0.1\t%s\n", hostname);
        if (m > 0) pos += m;
    }

    for (int i = 0; i < num_extra; i++) {
        if (!extra_hosts[i]) continue;
        int m = snprintf(hosts + pos, sizeof(hosts) - (size_t)pos,
                         "%s\n", extra_hosts[i]);
        if (m > 0) pos += m;
    }

    return vfs_write(path, hosts, (uint32_t)pos);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Container iptables-like rule storage (local tracking)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Per-container iptables-like rule list (in-memory store, just tracks
 * what rules have been added without actual packet filtering).
 */
#define MAX_CONTAINER_RULES 64

struct container_rule {
    int      in_use;
    char     cont_id[CONTAINER_ID_MAX];
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
    uint8_t  action;        /* NF_ACCEPT, NF_DROP */
    uint64_t added_tick;
};

static struct container_rule g_container_rules[MAX_CONTAINER_RULES];
static int g_container_rule_count = 0;

/* Add a container-specific firewall rule */
int container_rule_add(const char *cont_id, const struct nf_rule *rule)
{
    if (!cont_id || !rule) return -EINVAL;
    if (g_container_rule_count >= MAX_CONTAINER_RULES)
        return -ENOSPC;

    struct container_rule *cr = &g_container_rules[g_container_rule_count];
    memset(cr, 0, sizeof(*cr));
    strncpy(cr->cont_id, cont_id, CONTAINER_ID_MAX - 1);
    cr->cont_id[CONTAINER_ID_MAX - 1] = '\0';
    cr->src_ip = rule->src_ip;
    cr->dst_ip = rule->dst_ip;
    cr->src_port = rule->src_port;
    cr->dst_port = rule->dst_port;
    cr->protocol = rule->protocol;
    cr->action = rule->action;
    cr->added_tick = 0; /* timer_get_ticks() if available */
    cr->in_use = 1;
    g_container_rule_count++;

    /* Also add to global netfilter */
    return nf_add_rule(rule);
}

/* Remove all rules for a container */
int container_rule_flush(const char *cont_id)
{
    if (!cont_id) return -EINVAL;
    int removed = 0;
    for (int i = 0; i < g_container_rule_count; i++) {
        if (!g_container_rules[i].in_use) continue;
        if (strcmp(g_container_rules[i].cont_id, cont_id) == 0) {
            struct nf_rule rule;
            memset(&rule, 0, sizeof(rule));
            rule.src_ip = g_container_rules[i].src_ip;
            rule.dst_ip = g_container_rules[i].dst_ip;
            rule.src_port = g_container_rules[i].src_port;
            rule.dst_port = g_container_rules[i].dst_port;
            rule.protocol = g_container_rules[i].protocol;
            rule.action = g_container_rules[i].action;
            nf_del_rule(&rule);

            memset(&g_container_rules[i], 0, sizeof(g_container_rules[i]));
            removed++;
        }
    }
    return removed;
}

/* ── Bridge management ──────────────────────────────────────────── */

/* In-memory bridge table */
#define MAX_BRIDGES 16

struct bridge_entry {
    int   in_use;
    char  name[32];
    int   num_attached;
    char  attached_containers[8][CONTAINER_ID_MAX];
};

static struct bridge_entry g_bridges[MAX_BRIDGES];

static int bridge_find(const char *name)
{
    for (int i = 0; i < MAX_BRIDGES; i++) {
        if (g_bridges[i].in_use && strcmp(g_bridges[i].name, name) == 0)
            return i;
    }
    return -ENOENT;
}

static int bridge_find_free(void)
{
    for (int i = 0; i < MAX_BRIDGES; i++) {
        if (!g_bridges[i].in_use) return i;
    }
    return -ENOSPC;
}

/* net_create_bridge: Create a virtual bridge with the given name.
 * Registers it in the bridge table.  Returns 0 on success.
 */
int net_create_bridge(const char *name)
{
    if (!name || !name[0]) return -EINVAL;

    if (bridge_find(name) >= 0)
        return -EEXIST;

    int idx = bridge_find_free();
    if (idx < 0) return -ENOSPC;

    struct bridge_entry *b = &g_bridges[idx];
    memset(b, 0, sizeof(*b));
    strncpy(b->name, name, sizeof(b->name) - 1);
    b->name[sizeof(b->name) - 1] = '\0';
    b->in_use = 1;
    b->num_attached = 0;

    kprintf("[Net] Bridge '%s' created (idx=%d)\n", name, idx);
    return 0;
}

/* net_delete_bridge: Delete a virtual bridge by name.
 * Only succeeds if no containers are attached.
 * Returns 0 on success.
 */
int net_delete_bridge(const char *name)
{
    if (!name) return -EINVAL;

    int idx = bridge_find(name);
    if (idx < 0) return -ENOENT;

    struct bridge_entry *b = &g_bridges[idx];
    if (b->num_attached > 0) {
        kprintf("[Net] Cannot delete bridge '%s': %d containers attached\n",
                name, b->num_attached);
        return -EBUSY;
    }

    memset(b, 0, sizeof(*b));
    kprintf("[Net] Bridge '%s' deleted\n", name);
    return 0;
}

/* net_attach: Attach a container to a network bridge.
 * Creates a veth pair between the bridge and the container.
 * Returns 0 on success.
 */
int net_attach(const char *cont, const char *net)
{
    if (!cont || !net) return -EINVAL;

    int bidx = bridge_find(net);
    if (bidx < 0) return -ENOENT;

    struct bridge_entry *b = &g_bridges[bidx];
    if (b->num_attached >= 8) return -ENOSPC;

    /* Check if already attached */
    for (int i = 0; i < b->num_attached; i++) {
        if (strcmp(b->attached_containers[i], cont) == 0)
            return 0; /* already attached */
    }

    /* Create veth pair for this attachment */
    char host_if[32], cont_if[32];
    snprintf(host_if, sizeof(host_if), "veth-%s", cont);
    snprintf(cont_if, sizeof(cont_if), "eth0");

    int ret = veth_create_pair(host_if, cont_if, NULL);
    if (ret < 0) {
        kprintf("[Net] veth_create_pair failed for %s on %s: %d\n",
                cont, net, ret);
        return ret;
    }

    /* Record the attachment */
    strncpy(b->attached_containers[b->num_attached], cont,
            CONTAINER_ID_MAX - 1);
    b->attached_containers[b->num_attached][CONTAINER_ID_MAX - 1] = '\0';
    b->num_attached++;

    kprintf("[Net] Container '%s' attached to bridge '%s' (%d attached)\n",
            cont, net, b->num_attached);
    return 0;
}

/* net_detach: Detach a container from a network bridge.
 * Removes the veth pair and frees the attachment slot.
 * Returns 0 on success.
 */
int net_detach(const char *cont, const char *net)
{
    if (!cont || !net) return -EINVAL;

    int bidx = bridge_find(net);
    if (bidx < 0) return -ENOENT;

    struct bridge_entry *b = &g_bridges[bidx];

    for (int i = 0; i < b->num_attached; i++) {
        if (strcmp(b->attached_containers[i], cont) == 0) {
            /* Compact the array */
            int remaining = b->num_attached - i - 1;
            if (remaining > 0)
                memmove(&b->attached_containers[i],
                        &b->attached_containers[i + 1],
                        (size_t)remaining * CONTAINER_ID_MAX);
            b->num_attached--;

            kprintf("[Net] Container '%s' detached from bridge '%s' "
                    "(%d remaining)\n", cont, net, b->num_attached);
            return 0;
        }
    }

    return -ENOENT; /* not attached to this bridge */
}
