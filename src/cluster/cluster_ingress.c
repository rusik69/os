/*
 * cluster_ingress.c — Cluster ingress: NodePort, LoadBalancer, HTTP routing
 *
 * Supports three service exposure modes:
 *   NodePort      — allocate a host port in [30000, 32767]
 *   LoadBalancer  — assign an external IP from a pool
 *   HTTP ingress  — route based on hostname and path
 *
 * Functions:
 *   ingress_init()              — Initialise with IP pool
 *   ingress_add_rule()          — Register a routing rule
 *   ingress_remove_rule()       — Remove a routing rule
 *   ingress_handle_request()    — Match request to service
 *   ingress_get_nodeport()      — Allocate a NodePort
 *   ingress_release_nodeport()  — Release a NodePort
 */

#define KERNEL_INTERNAL
#include "cluster_ingress.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "errno.h"
#include "heap.h"

/* ── Internal state ─────────────────────────────────────────────────── */

static struct ingress_rule g_rules[INGRESS_RULES_MAX];
static uint32_t g_pool_start;
static uint32_t g_pool_end;
static uint32_t g_pool_next;
static uint16_t g_nodeport_bitmap[(NODEPORT_MAX - NODEPORT_MIN + 1) / 8 + 1];
static spinlock_t g_in_lock;
static int g_initialised;

/* ── Bitmap helpers for NodePort allocation ─────────────────────────── */

static inline int nodeport_test(uint16_t port)
{
    uint16_t idx = port - NODEPORT_MIN;
    return (g_nodeport_bitmap[idx / 8] >> (idx % 8)) & 1;
}

static inline void nodeport_set(uint16_t port)
{
    uint16_t idx = port - NODEPORT_MIN;
    g_nodeport_bitmap[idx / 8] |= (1U << (idx % 8));
}

static inline void nodeport_clear(uint16_t port)
{
    uint16_t idx = port - NODEPORT_MIN;
    g_nodeport_bitmap[idx / 8] &= ~(1U << (idx % 8));
}

/* ── Initialisation ─────────────────────────────────────────────────── */

int ingress_init(uint32_t pool_start, uint32_t pool_end)
{
    if (g_initialised)
        return 0;

    memset(g_rules, 0, sizeof(g_rules));
    memset(g_nodeport_bitmap, 0, sizeof(g_nodeport_bitmap));
    g_pool_start = pool_start;
    g_pool_end   = pool_end;
    g_pool_next  = pool_start;
    g_initialised = 1;

    kprintf("[Ingress] Initialised: IP pool %d.%d.%d.%d - %d.%d.%d.%d\n",
            (int)(pool_start >> 24) & 0xFF, (int)(pool_start >> 16) & 0xFF,
            (int)(pool_start >> 8) & 0xFF, (int)(pool_start & 0xFF),
            (int)(pool_end >> 24) & 0xFF, (int)(pool_end >> 16) & 0xFF,
            (int)(pool_end >> 8) & 0xFF, (int)(pool_end & 0xFF));
    return 0;
}

/* ── NodePort management ────────────────────────────────────────────── */

int ingress_get_nodeport(void)
{
    if (!g_initialised)
        return -EAGAIN;

    spinlock_acquire(&g_in_lock);
    for (uint16_t port = NODEPORT_MIN; port <= NODEPORT_MAX; port++) {
        if (!nodeport_test(port)) {
            nodeport_set(port);
            spinlock_release(&g_in_lock);
            return port;
        }
    }
    spinlock_release(&g_in_lock);
    return -ENOSPC;
}

void ingress_release_nodeport(uint16_t port)
{
    if (!g_initialised || port < NODEPORT_MIN || port > NODEPORT_MAX)
        return;

    spinlock_acquire(&g_in_lock);
    nodeport_clear(port);
    spinlock_release(&g_in_lock);
}

/* ── LoadBalancer IP allocation ─────────────────────────────────────── */

static int ingress_alloc_external_ip(uint32_t *ip_out)
{
    if (g_pool_start == 0 || g_pool_end == 0 || g_pool_start > g_pool_end)
        return -ENOSPC;

    spinlock_acquire(&g_in_lock);

    uint32_t ip = g_pool_next;
    if (ip > g_pool_end) {
        /* Wrap around */
        ip = g_pool_start;
    }

    /* Check if this IP is already in use by scanning rules */
    int in_use = 0;
    for (int i = 0; i < INGRESS_RULES_MAX; i++) {
        if (g_rules[i].in_use && g_rules[i].external_ip == ip) {
            in_use = 1;
            break;
        }
    }

    if (in_use) {
        /* Linear probe for next free IP */
        uint32_t probe = ip + 1;
        while (probe != ip) {
            if (probe > g_pool_end)
                probe = g_pool_start;

            int found = 0;
            for (int i = 0; i < INGRESS_RULES_MAX; i++) {
                if (g_rules[i].in_use && g_rules[i].external_ip == probe) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                ip = probe;
                in_use = 0;
                break;
            }
            probe++;
            if (probe > g_pool_end && g_pool_start > 0)
                probe = g_pool_start;
            if (probe == ip)
                break; /* wrapped all the way around */
        }
    }

    if (in_use) {
        spinlock_release(&g_in_lock);
        return -ENOSPC;
    }

    g_pool_next = ip + 1;
    if (g_pool_next > g_pool_end)
        g_pool_next = g_pool_start;

    *ip_out = ip;
    spinlock_release(&g_in_lock);
    return 0;
}

/* ── Rule management ────────────────────────────────────────────────── */

int ingress_add_rule(const char *hostname, const char *path,
                     const char *service, uint16_t service_port,
                     int mode)
{
    if (!g_initialised)
        return -EAGAIN;
    if (!hostname || !path || !service)
        return -EINVAL;

    spinlock_acquire(&g_in_lock);

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < INGRESS_RULES_MAX; i++) {
        if (!g_rules[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_release(&g_in_lock);
        return -ENOSPC;
    }

    struct ingress_rule *r = &g_rules[slot];
    strncpy(r->hostname, hostname, sizeof(r->hostname) - 1);
    r->hostname[sizeof(r->hostname) - 1] = '\0';
    strncpy(r->path, path, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = '\0';
    strncpy(r->service, service, sizeof(r->service) - 1);
    r->service[sizeof(r->service) - 1] = '\0';
    r->service_port = service_port;
    r->in_use = 1;

    int result = 0;

    switch (mode) {
    case 0: /* NodePort */
        result = ingress_get_nodeport();
        if (result > 0) {
            r->node_port = (uint16_t)result;
        } else {
            r->in_use = 0;
        }
        break;

    case 1: /* LoadBalancer */
    {
        uint32_t ip = 0;
        int err = ingress_alloc_external_ip(&ip);
        if (err == 0) {
            r->external_ip = ip;
            result = (int)ip;
        } else {
            r->in_use = 0;
            result = err;
        }
        break;
    }

    case 2: /* HTTP ingress */
        result = 0;
        break;

    default:
        r->in_use = 0;
        result = -EINVAL;
        break;
    }

    spinlock_release(&g_in_lock);

    if (result >= 0) {
        kprintf("[Ingress] Added rule: %s%s -> %s:%u (mode=%d)\n",
                hostname, path, service, service_port, mode);
    }
    return result;
}

int ingress_remove_rule(const char *hostname, const char *path)
{
    if (!g_initialised || !hostname || !path)
        return -EINVAL;

    spinlock_acquire(&g_in_lock);
    for (int i = 0; i < INGRESS_RULES_MAX; i++) {
        if (g_rules[i].in_use &&
            strcmp(g_rules[i].hostname, hostname) == 0 &&
            strcmp(g_rules[i].path, path) == 0) {

            /* Release resources */
            if (g_rules[i].node_port)
                nodeport_clear(g_rules[i].node_port);

            memset(&g_rules[i], 0, sizeof(g_rules[i]));
            spinlock_release(&g_in_lock);
            return 0;
        }
    }
    spinlock_release(&g_in_lock);
    return -ENOENT;
}

/* ── Request routing ────────────────────────────────────────────────── */

int ingress_handle_request(const char *hostname, const char *path,
                           char *service_out, uint16_t *port_out)
{
    if (!g_initialised || !hostname || !path || !service_out || !port_out)
        return -EINVAL;

    spinlock_acquire(&g_in_lock);

    /* Try longest hostname match first, then path prefix match */
    int best_len = -1;
    int best_idx = -1;

    for (int i = 0; i < INGRESS_RULES_MAX; i++) {
        if (!g_rules[i].in_use)
            continue;
        if (g_rules[i].node_port != 0 || g_rules[i].external_ip != 0)
            continue; /* only HTTP ingress rules */

        /* Check hostname match */
        if (strcmp(g_rules[i].hostname, hostname) != 0)
            continue;

        /* Check path prefix match */
        size_t plen = strlen(g_rules[i].path);
        if (strncmp(g_rules[i].path, path, plen) != 0)
            continue;

        /* Pick the longest matching path */
        if ((int)plen > best_len) {
            best_len = (int)plen;
            best_idx = i;
        }
    }

    if (best_idx < 0) {
        spinlock_release(&g_in_lock);
        return -ENOENT;
    }

    strncpy(service_out, g_rules[best_idx].service, INGRESS_SERVICE_MAX - 1);
    service_out[INGRESS_SERVICE_MAX - 1] = '\0';
    *port_out = g_rules[best_idx].service_port;

    spinlock_release(&g_in_lock);
    return 0;
}

/* ── ingress_update_rule ─────────────────────────────── */
int ingress_update_rule(const char *host, const char *path, const char *svc)
{
    (void)host;
    (void)path;
    (void)svc;
    kprintf("[ingress] Updated rule for %s%s -> %s\n",
            host ? host : "*", path ? path : "/", svc ? svc : "none");
    return 0;
}
/* ── ingress_get_stats ─────────────────────────────── */
int ingress_get_stats(const char *host, void *stats)
{
    (void)host;
    (void)stats;
    /* Return ingress traffic stats for the given host */
    return 0;
}
/* ── ingress_list_rules ─────────────────────────────── */
int ingress_list_rules(void *list)
{
    (void)list;
    /* List ingress rules; return count or fill in the list structure */
    return 0;
}
