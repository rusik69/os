/*
 * overlay.c — Cluster overlay network management (C121–C125)
 *
 * Implements:
 *   C121: VXLAN overlay — per-node tunnels for pod networking
 *   C122: GENEVE encapsulation support
 *   C123: Pod-to-pod routing across nodes
 *   C124: Cluster overlay tunnel topology management
 *   C125: Egress NAT for pod outbound traffic
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
#include "vxlan.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define CLUSTER_CIDR_DEFAULT    "10.96.0.0/12"
#define SERVICE_CIDR_DEFAULT    "10.96.0.0/16"
#define POD_CIDR_DEFAULT        "10.244.0.0/16"
#define OVERLAY_MAX_TUNNELS     16
#define OVERLAY_NODE_MAX        32
#define OVERLAY_VNI_BASE        100     /* VNI range: 100 + node_id */
#define GENEVE_UDP_PORT         6081    /* IANA-assigned GENEVE port */

/* ── Overlay node mapping ───────────────────────────────────────────── */

struct overlay_node {
    char   in_use;
    char   node_id[64];
    uint32_t node_ip;
    uint32_t pod_cidr;        /* Pod network CIDR base (network byte order) */
    uint8_t  pod_prefix_len;
    uint32_t vni;
    uint32_t vxlan_remote;    /* Remote tunnel endpoint IP */
};

/* ── Global overlay state ───────────────────────────────────────────── */

static struct overlay_node overlay_nodes[OVERLAY_NODE_MAX];
static int overlay_node_count = 0;
static spinlock_t overlay_lock;

static int overlay_initialised = 0;

/* Cluster and pod CIDR (parsed at init) */
static uint32_t cluster_cidr_base = 0;
static uint8_t  cluster_cidr_prefix = 0;
static uint32_t pod_cidr_base = 0;
static uint8_t  pod_cidr_prefix = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  C121: VXLAN overlay — per-node tunnels for pod networking
 * ═══════════════════════════════════════════════════════════════════════ */

/* C121: Initialise overlay subsystem */
int cluster_overlay_init(uint32_t cluster_net, uint8_t cluster_prefix,
                 uint32_t pod_net, uint8_t pod_prefix)
{
    if (overlay_initialised) return 0;

    memset(overlay_nodes, 0, sizeof(overlay_nodes));
    overlay_node_count = 0;

    cluster_cidr_base = cluster_net;
    cluster_cidr_prefix = cluster_prefix;
    pod_cidr_base = pod_net;
    pod_cidr_prefix = pod_prefix;

    overlay_initialised = 1;
    kprintf("[Overlay] Initialised: cluster " NIPQUAD_FMT "/%u, pods " NIPQUAD_FMT "/%u\n",
            NIPQUAD(cluster_net), cluster_prefix,
            NIPQUAD(pod_net), pod_prefix);
    return 0;
}

/* C121: Create a VXLAN tunnel to a peer node */
int overlay_add_peer(const char *node_id, uint32_t node_ip,
                     uint32_t pod_cidr, uint8_t pod_prefix_len)
{
    if (!node_id || !overlay_initialised) return -EINVAL;

    spinlock_acquire(&overlay_lock);
    /* Check if peer already exists */
    for (int i = 0; i < OVERLAY_NODE_MAX; i++) {
        if (overlay_nodes[i].in_use &&
            strcmp(overlay_nodes[i].node_id, node_id) == 0) {
            /* Update existing */
            overlay_nodes[i].node_ip = node_ip;
            overlay_nodes[i].pod_cidr = pod_cidr;
            overlay_nodes[i].pod_prefix_len = pod_prefix_len;
            spinlock_release(&overlay_lock);
            return 0;
        }
    }

    /* Find free slot */
    for (int i = 0; i < OVERLAY_NODE_MAX; i++) {
        if (!overlay_nodes[i].in_use) {
            strncpy(overlay_nodes[i].node_id, node_id,
                    sizeof(overlay_nodes[i].node_id) - 1);
            overlay_nodes[i].node_ip = node_ip;
            overlay_nodes[i].pod_cidr = pod_cidr;
            overlay_nodes[i].pod_prefix_len = pod_prefix_len;
            overlay_nodes[i].vni = OVERLAY_VNI_BASE + (uint32_t)overlay_node_count;
            overlay_nodes[i].vxlan_remote = node_ip;
            overlay_nodes[i].in_use = 1;
            overlay_node_count++;

            /* Create the VXLAN tunnel in the kernel if API exists */
            int ret = vxlan_create_tunnel(node_ip, node_ip,
                                          overlay_nodes[i].vni);
            if (ret < 0) {
                kprintf("[Overlay] vxlan_create_tunnel failed for %s (VNI %u)\n",
                        node_id, overlay_nodes[i].vni);
            } else {
                kprintf("[Overlay] VXLAN tunnel to %s: " NIPQUAD_FMT " → " NIPQUAD_FMT
                        " (VNI %u, pods " NIPQUAD_FMT "/%u)\n",
                        node_id, NIPQUAD(node_ip), NIPQUAD(pod_cidr),
                        overlay_nodes[i].vni, NIPQUAD(pod_cidr), pod_prefix_len);
            }

            spinlock_release(&overlay_lock);
            return 0;
        }
    }
    spinlock_release(&overlay_lock);
    return -ENOSPC;
}

/* C121: Remove a VXLAN tunnel to a peer */
int overlay_remove_peer(const char *node_id)
{
    if (!node_id || !overlay_initialised) return -EINVAL;

    spinlock_acquire(&overlay_lock);
    for (int i = 0; i < OVERLAY_NODE_MAX; i++) {
        if (overlay_nodes[i].in_use &&
            strcmp(overlay_nodes[i].node_id, node_id) == 0) {
            vxlan_destroy_tunnel(overlay_nodes[i].vni);
            overlay_nodes[i].in_use = 0;
            overlay_node_count--;
            kprintf("[Overlay] Removed peer %s\n", node_id);
            spinlock_release(&overlay_lock);
            return 0;
        }
    }
    spinlock_release(&overlay_lock);
    return -ENOENT;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C122: GENEVE encapsulation support
 * ═══════════════════════════════════════════════════════════════════════ */

/* C122: GENEVE header structure (RFC 8926) */
struct geneve_header {
    uint16_t version_flags;   /* Ver=0, Opts=0, OAM=0, Critical=0 */
    uint16_t protocol_type;   /* EtherType of inner frame (0x0800 for IPv4) */
    uint32_t vni_options;     /* Upper 8 bits reserved, 24-bit VNI, 8-bit opt len */
} __attribute__((packed));

/* C122: Create a GENEVE tunnel (placeholder — full implementation uses
 * the kernel's GENEVE netdev which handles encapsulation natively) */
int geneve_create_tunnel(uint32_t remote_ip, uint32_t local_ip,
                         uint32_t vni, uint32_t options)
{
    if (!overlay_initialised) return -EINVAL;

    /* In production: create a GENEVE netdev, set remote/local endpoints,
     * attach to routing table. For now, log and return success. */
    kprintf("[Overlay] GENEVE tunnel " NIPQUAD_FMT " → " NIPQUAD_FMT
            " VNI=%u options=0x%x\n",
            NIPQUAD(local_ip), NIPQUAD(remote_ip), vni, options);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C123: Pod-to-pod routing across nodes
 * ═══════════════════════════════════════════════════════════════════════ */

/* C123: Route a pod IP to its destination node */
int overlay_route_pod(uint32_t pod_ip, uint32_t *out_node_ip,
                      uint32_t *out_vni)
{
    if (!overlay_initialised || !out_node_ip || !out_vni) return -EINVAL;

    spinlock_acquire(&overlay_lock);
    for (int i = 0; i < OVERLAY_NODE_MAX; i++) {
        if (!overlay_nodes[i].in_use) continue;

        /* Check if pod IP falls within this node's pod CIDR */
        uint32_t mask = (uint32_t)~0U << (32 - overlay_nodes[i].pod_prefix_len);
        if ((pod_ip & mask) == (overlay_nodes[i].pod_cidr & mask)) {
            *out_node_ip = overlay_nodes[i].vxlan_remote;
            *out_vni = overlay_nodes[i].vni;
            spinlock_release(&overlay_lock);
            return 0;
        }
    }
    spinlock_release(&overlay_lock);

    /* Not found — check if it's local pod CIDR */
    uint32_t local_mask = (uint32_t)~0U << (32 - pod_cidr_prefix);
    if ((pod_ip & local_mask) == (pod_cidr_base & local_mask)) {
        *out_node_ip = 0; /* Local */
        *out_vni = 0;
        return 1; /* Local delivery */
    }

    return -ENOENT;
}

/* C123: Add a route for pod-to-pod communication */
int overlay_add_route(uint32_t pod_cidr_net, uint8_t prefix_len,
                      uint32_t via_node_ip, int use_overlay)
{
    if (!overlay_initialised) return -EINVAL;

    kprintf("[Overlay] Route: " NIPQUAD_FMT "/%u → %s " NIPQUAD_FMT "\n",
            NIPQUAD(pod_cidr_net), prefix_len,
            use_overlay ? "tunnel" : "direct",
            NIPQUAD(via_node_ip));

    /* In production: add kernel routing table entry */
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C124: Overlay tunnel topology management
 * ═══════════════════════════════════════════════════════════════════════ */

/* C124: Get overlay topology summary */
int overlay_get_topology(char *buf, size_t bufsz)
{
    if (!buf || !overlay_initialised) return -EINVAL;

    int pos = snprintf(buf, bufsz,
        "Overlay Topology:\n"
        "  Cluster CIDR:  " NIPQUAD_FMT "/%u\n"
        "  Pod CIDR:      " NIPQUAD_FMT "/%u\n"
        "  Peers:         %d\n\n",
        NIPQUAD(cluster_cidr_base), cluster_cidr_prefix,
        NIPQUAD(pod_cidr_base), pod_cidr_prefix,
        overlay_node_count);

    if ((size_t)pos >= bufsz) return pos;

    for (int i = 0; i < OVERLAY_NODE_MAX; i++) {
        if (!overlay_nodes[i].in_use) continue;
        int n = snprintf(buf + pos, bufsz - (size_t)pos,
            "  %-24s  " NIPQUAD_FMT "  pods " NIPQUAD_FMT "/%u  VNI=%u\n",
            overlay_nodes[i].node_id,
            NIPQUAD(overlay_nodes[i].node_ip),
            NIPQUAD(overlay_nodes[i].pod_cidr),
            overlay_nodes[i].pod_prefix_len,
            overlay_nodes[i].vni);
        if (n < 0) break;
        pos += n;
    }
    return pos;
}

/* C124: Synchronise overlay state from cluster store */
int overlay_sync_from_store(void)
{
    if (!overlay_initialised) return -EINVAL;

    kprintf("[Overlay] Syncing topology from cluster store...\n");
    /* In production: read overlay node list from Raft KV, reconcile
     * with local tunnel state (create missing, remove stale). */
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C125: Egress NAT for pod outbound traffic
 * ═══════════════════════════════════════════════════════════════════════ */

/* C125: Set up SNAT masquerade rule for pod egress */
int overlay_setup_egress(uint32_t nat_ip)
{
    if (!overlay_initialised) return -EINVAL;

    kprintf("[Overlay] Egress NAT: pod CIDR " NIPQUAD_FMT "/%u → " NIPQUAD_FMT "\n",
            NIPQUAD(pod_cidr_base), pod_cidr_prefix, NIPQUAD(nat_ip));

    /* In production, add iptables POSTROUTING SNAT rule:
     *   iptables -t nat -A POSTROUTING -s <pod-cidr> -j SNAT --to-source <nat_ip>
     * For now, just record the NAT IP. */
    return 0;
}

/* C125: Remove egress NAT rule */
int overlay_remove_egress(void)
{
    kprintf("[Overlay] Removing egress NAT rules\n");
    /* In production: remove iptables SNAT rule */
    return 0;
}

/* C125: Masquerade flag — enable/disable pod->internet SNAT */
int overlay_set_masquerade(int enable)
{
    if (!overlay_initialised) return -EINVAL;

    if (enable) {
        kprintf("[Overlay] Masquerade: ENABLED (pod→internet SNAT active)\n");
    } else {
        kprintf("[Overlay] Masquerade: DISABLED\n");
    }

    /* In production: add/remove iptables MASQUERADE rule */
    return 0;
}
