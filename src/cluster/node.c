/*
 * node.c — Cluster node management (C112–C115)
 *
 * Implements:
 *   C112: Node registration and health reporting
 *   C113: Leader election for orchestration controller
 *   C114: Service endpoint synchronization
 *   C115: Pod assignment reconciliation (reconciler loop)
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"
#include "socket.h"
#include "net.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define NODE_ID_MAX            64
#define NODE_HOSTNAME_MAX      128
#define NODE_MAX               32
#define NODE_HEARTBEAT_INTERVAL 15000   /* ms between heartbeats */
#define NODE_HEALTH_TIMEOUT    45000   /* ms before marking node unhealthy */
#define ENDPOINT_MAX           512
#define RECONCILE_INTERVAL     30000   /* ms between reconciliation loops */

/* Node status flags */
#define NODE_STATUS_READY       0
#define NODE_STATUS_NOT_READY   1
#define NODE_STATUS_UNKNOWN     2
#define NODE_STATUS_DISK_PRESSURE  3
#define NODE_STATUS_MEM_PRESSURE   4

/* ── Node descriptor ────────────────────────────────────────────────── */

struct cluster_node {
    char   in_use;
    char   id[NODE_ID_MAX];
    char   hostname[NODE_HOSTNAME_MAX];
    uint32_t ip;
    uint16_t port;

    /* Resource capacity */
    uint64_t cpu_cores_total;
    uint64_t cpu_cores_used;
    uint64_t memory_total;       /* bytes */
    uint64_t memory_used;
    uint64_t disk_total;         /* bytes */
    uint64_t disk_used;
    uint32_t container_count;

    /* Status */
    int    status;               /* NODE_STATUS_* */
    uint64_t last_heartbeat;
    int    incarnation;

    /* Capabilities */
    uint32_t capabilities;       /* Bitmask */
};

/* ── Endpoint (pod IP:port for service routing) ──────────────────────── */

struct service_endpoint {
    char   in_use;
    char   service_name[128];
    char   pod_id[64];
    uint32_t pod_ip;
    uint16_t pod_port;
    uint16_t target_port;
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct cluster_node nodes[NODE_MAX];
static int node_count = 0;
static int local_node_idx = -1;
static spinlock_t node_lock;

static struct service_endpoint endpoints[ENDPOINT_MAX];
static int endpoint_count = 0;
static spinlock_t endpoint_lock;

static int node_initialised = 0;

/* Orchestration leader tracking */
static char leader_id[NODE_ID_MAX];
static uint64_t leader_term = 0;
static uint64_t leader_last_seen = 0;
static int is_leader = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  C112: Node registration and health reporting
 * ═══════════════════════════════════════════════════════════════════════ */

/* C112: Register this node in the cluster */
int node_register(const char *node_id, const char *hostname,
                  uint32_t ip, uint16_t port,
                  uint64_t cpu_cores, uint64_t memory_total_bytes,
                  uint64_t disk_total_bytes)
{
    if (!node_id || !hostname) return -EINVAL;

    spinlock_acquire(&node_lock);
    if (!node_initialised) {
        memset(nodes, 0, sizeof(nodes));
        node_initialised = 1;
    }

    /* Check for duplicate */
    for (int i = 0; i < NODE_MAX; i++) {
        if (!nodes[i].in_use) continue;
        if (strcmp(nodes[i].id, node_id) == 0) {
            /* Already registered — update info */
            nodes[i].ip = ip;
            nodes[i].port = port;
            nodes[i].cpu_cores_total = cpu_cores;
            nodes[i].memory_total = memory_total_bytes;
            nodes[i].disk_total = disk_total_bytes;
            nodes[i].last_heartbeat = timer_get_ms();
            nodes[i].incarnation++;
            local_node_idx = i;
            spinlock_release(&node_lock);
            kprintf("[Node] Updated registration for %s (idx %d)\n", node_id, i);
            return 0;
        }
    }

    /* Find free slot */
    for (int i = 0; i < NODE_MAX; i++) {
        if (!nodes[i].in_use) {
            strncpy(nodes[i].id, node_id, NODE_ID_MAX - 1);
            strncpy(nodes[i].hostname, hostname, NODE_HOSTNAME_MAX - 1);
            nodes[i].ip = ip;
            nodes[i].port = port;
            nodes[i].cpu_cores_total = cpu_cores;
            nodes[i].memory_total = memory_total_bytes;
            nodes[i].disk_total = disk_total_bytes;
            nodes[i].cpu_cores_used = 0;
            nodes[i].memory_used = 0;
            nodes[i].disk_used = 0;
            nodes[i].container_count = 0;
            nodes[i].status = NODE_STATUS_READY;
            nodes[i].last_heartbeat = timer_get_ms();
            nodes[i].incarnation = 0;
            nodes[i].in_use = 1;
            local_node_idx = i;
            node_count++;
            spinlock_release(&node_lock);
            kprintf("[Node] Registered node %s (%s) at " NIPQUAD_FMT ":%d\n",
                    node_id, hostname, NIPQUAD(ip), port);
            return 0;
        }
    }

    spinlock_release(&node_lock);
    return -ENOSPC;
}

/* C112: Send heartbeat — update resource usage */
int node_heartbeat(uint64_t cpu_used, uint64_t mem_used,
                   uint64_t disk_used, uint32_t container_count)
{
    if (local_node_idx < 0) return -EINVAL;

    spinlock_acquire(&node_lock);
    struct cluster_node *n = &nodes[local_node_idx];
    n->cpu_cores_used = cpu_used;
    n->memory_used = mem_used;
    n->disk_used = disk_used;
    n->container_count = container_count;
    n->last_heartbeat = timer_get_ms();
    n->incarnation++;
    spinlock_release(&node_lock);

    return 0;
}

/* C112: Get node status information */
int node_get_info(const char *node_id, struct cluster_node *out)
{
    if (!node_id || !out) return -EINVAL;

    spinlock_acquire(&node_lock);
    for (int i = 0; i < NODE_MAX; i++) {
        if (!nodes[i].in_use || strcmp(nodes[i].id, node_id) != 0) continue;
        memcpy(out, &nodes[i], sizeof(*out));
        spinlock_release(&node_lock);
        return 0;
    }
    spinlock_release(&node_lock);
    return -ENOENT;
}

/* C112: Check node health status */
int node_check_health(const char *node_id)
{
    if (!node_id) return -EINVAL;

    uint64_t now = timer_get_ms();
    spinlock_acquire(&node_lock);
    for (int i = 0; i < NODE_MAX; i++) {
        if (!nodes[i].in_use || strcmp(nodes[i].id, node_id) != 0) continue;

        if (now - nodes[i].last_heartbeat > NODE_HEALTH_TIMEOUT) {
            nodes[i].status = NODE_STATUS_UNKNOWN;
            spinlock_release(&node_lock);
            return NODE_STATUS_UNKNOWN;
        }
        spinlock_release(&node_lock);
        return nodes[i].status;
    }
    spinlock_release(&node_lock);
    return -ENOENT;
}

/* C112: List all cluster nodes */
int node_list_all(char *buf, size_t bufsz)
{
    if (!buf) return -EINVAL;

    int pos = 0;
    spinlock_acquire(&node_lock);
    for (int i = 0; i < NODE_MAX; i++) {
        if (!nodes[i].in_use) continue;

        const char *status_str = "ready";
        if (nodes[i].status == NODE_STATUS_NOT_READY) status_str = "not-ready";
        else if (nodes[i].status == NODE_STATUS_UNKNOWN) status_str = "unknown";

        if ((size_t)pos >= bufsz) break;
        int n = snprintf(buf + pos, bufsz - (size_t)pos,
                         "%-24s  %-16s  " NIPQUAD_FMT ":%5d  %s  cpu=%lu/%lu  mem=%lu/%lu  pods=%u\n",
                         nodes[i].id, nodes[i].hostname,
                         NIPQUAD(nodes[i].ip), nodes[i].port,
                         status_str,
                         nodes[i].cpu_cores_used, nodes[i].cpu_cores_total,
                         nodes[i].memory_used, nodes[i].memory_total,
                         nodes[i].container_count);
        if (n < 0) break;
        pos += n;
    }
    spinlock_release(&node_lock);
    return pos;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C113: Leader election for orchestration controller
 * ═══════════════════════════════════════════════════════════════════════ */

/* C113: Announce candidacy for orchestration leader */
int node_announce_candidacy(const char *node_id, uint64_t term)
{
    if (!node_id) return -EINVAL;

    spinlock_acquire(&node_lock);
    if (term <= leader_term) {
        /* We already have a leader with higher or equal term */
        spinlock_release(&node_lock);
        return -EAGAIN;
    }

    leader_term = term;
    strncpy(leader_id, node_id, NODE_ID_MAX - 1);
    leader_last_seen = timer_get_ms();
    is_leader = (strcmp(node_id, nodes[local_node_idx].id) == 0) ? 1 : 0;

    spinlock_release(&node_lock);
    kprintf("[Node] Node %s announced candidacy (term %lu) — %s\n",
            node_id, term, is_leader ? "ELECTED LEADER" : "follower");
    return 0;
}

/* C113: Check if this node is the orchestration leader */
int node_is_leader(void)
{
    return is_leader;
}

/* C113: Get current leader */
int node_get_leader(char *leader_out, size_t maxlen)
{
    if (!leader_out) return -EINVAL;

    spinlock_acquire(&node_lock);
    strncpy(leader_out, leader_id, maxlen - 1);
    spinlock_release(&node_lock);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C114: Service endpoint synchronization
 * ═══════════════════════════════════════════════════════════════════════ */

/* C114: Add a service endpoint (pod IP:port for a service) */
int endpoint_add(const char *service_name, const char *pod_id,
                 uint32_t pod_ip, uint16_t pod_port, uint16_t target_port)
{
    if (!service_name || !pod_id) return -EINVAL;

    spinlock_acquire(&endpoint_lock);
    for (int i = 0; i < ENDPOINT_MAX; i++) {
        if (endpoints[i].in_use &&
            strcmp(endpoints[i].service_name, service_name) == 0 &&
            strcmp(endpoints[i].pod_id, pod_id) == 0) {
            /* Already exists — update */
            endpoints[i].pod_ip = pod_ip;
            endpoints[i].pod_port = pod_port;
            endpoints[i].target_port = target_port;
            spinlock_release(&endpoint_lock);
            return 0;
        }
    }

    /* Find free slot */
    for (int i = 0; i < ENDPOINT_MAX; i++) {
        if (!endpoints[i].in_use) {
            strncpy(endpoints[i].service_name, service_name,
                    sizeof(endpoints[i].service_name) - 1);
            strncpy(endpoints[i].pod_id, pod_id,
                    sizeof(endpoints[i].pod_id) - 1);
            endpoints[i].pod_ip = pod_ip;
            endpoints[i].pod_port = pod_port;
            endpoints[i].target_port = target_port;
            endpoints[i].in_use = 1;
            endpoint_count++;
            spinlock_release(&endpoint_lock);
            return 0;
        }
    }
    spinlock_release(&endpoint_lock);
    return -ENOSPC;
}

/* C114: Remove a service endpoint */
int endpoint_remove(const char *service_name, const char *pod_id)
{
    if (!service_name || !pod_id) return -EINVAL;

    spinlock_acquire(&endpoint_lock);
    for (int i = 0; i < ENDPOINT_MAX; i++) {
        if (endpoints[i].in_use &&
            strcmp(endpoints[i].service_name, service_name) == 0 &&
            strcmp(endpoints[i].pod_id, pod_id) == 0) {
            endpoints[i].in_use = 0;
            endpoint_count--;
            spinlock_release(&endpoint_lock);
            return 0;
        }
    }
    spinlock_release(&endpoint_lock);
    return -ENOENT;
}

/* C114: Remove all endpoints for a given pod */
int endpoint_remove_pod(const char *pod_id)
{
    if (!pod_id) return -EINVAL;

    int removed = 0;
    spinlock_acquire(&endpoint_lock);
    for (int i = 0; i < ENDPOINT_MAX; i++) {
        if (endpoints[i].in_use && strcmp(endpoints[i].pod_id, pod_id) == 0) {
            endpoints[i].in_use = 0;
            endpoint_count--;
            removed++;
        }
    }
    spinlock_release(&endpoint_lock);
    return removed;
}

/* C114: Get all endpoints for a service */
int endpoint_get_for_service(const char *service_name,
                             struct service_endpoint *out, int max)
{
    if (!service_name || !out) return -EINVAL;

    int count = 0;
    spinlock_acquire(&endpoint_lock);
    for (int i = 0; i < ENDPOINT_MAX && count < max; i++) {
        if (endpoints[i].in_use &&
            strcmp(endpoints[i].service_name, service_name) == 0) {
            memcpy(&out[count++], &endpoints[i], sizeof(struct service_endpoint));
        }
    }
    spinlock_release(&endpoint_lock);
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C115: Pod assignment reconciliation
 * ═══════════════════════════════════════════════════════════════════════ */

/* C115: Reconciliation loop — ensure assigned pods are running */
int reconciler_tick(void)
{
    if (!node_initialised || local_node_idx < 0) return 0;

    /* In production, this reads desired pod assignments from cluster store
     * and compares with locally running containers.
     * Simplified: just report status for now. */
    kprintf("[Reconciler] Tick — node %s heartbeat check\n",
            nodes[local_node_idx].id);

    /* Check if we need to re-register (leader change, etc.) */
    uint64_t now = timer_get_ms();
    if (now - nodes[local_node_idx].last_heartbeat > NODE_HEARTBEAT_INTERVAL) {
        kprintf("[Reconciler] Heartbeat overdue on node %s — re-registering\n",
                nodes[local_node_idx].id);
    }

    /* Check orchestration leader health */
    if (leader_id[0] && now - leader_last_seen > NODE_HEALTH_TIMEOUT) {
        kprintf("[Reconciler] Leader %s not seen for %lu ms — triggering re-election\n",
                leader_id, now - leader_last_seen);
        leader_id[0] = '\0';
        is_leader = 0;
    }

    return 0;
}
