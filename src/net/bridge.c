/* bridge.c — Learning bridge with netdevice-based frame forwarding,
 * IGMP snooping for multicast filtering (Item 163),
 * and 802.1D-1998 Spanning Tree Protocol (STP, Item 164).
 *
 * Implements:
 *   - MAC learning with ageing
 *   - Unicast forwarding via FDB lookup
 *   - Broadcast flooding to all ports except ingress
 *   - STP port state enforcement (blocking/listening/learning/forwarding)
 *   - Multicast forwarding with IGMP snooping:
 *     When an IGMP Membership Report is seen on a port, that port
 *     is added to the multicast group's port_mask.  Subsequent
 *     multicast frames are only forwarded to subscribers.
 *   - IGMP Leave messages remove a port from the group.
 *   - Ageing removes stale multicast entries (like FDB ageing). */

#define KERNEL_INTERNAL
#include "bridge.h"
#include "stp.h"
#include "netdevice.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "timers.h"
#include "export.h"
#include "net_igmp.h"    /* IGMP type constants */

static struct bridge g_bridge;
static int g_stp_timer_id = -1;  /* timer handle for STP tick, used for cleanup */

/* ── STP timer callback ──────────────────────────────────────────── */

static void stp_timer_cb(void *arg) {
    (void)arg;
    stp_tick();
    /* Re-schedule for next tick; save new timer ID for cleanup */
    g_stp_timer_id = timer_schedule(stp_timer_cb, NULL, 1);
}

/* ── Initialisation / Cleanup ────────────────────────────────────── */

int bridge_init(void) {
    if (g_bridge.initialized) return -1;
    memset(&g_bridge, 0, sizeof(g_bridge));
    g_bridge.initialized = 1;

    /* Initialise STP with the bridge MAC and default priority */
    uint8_t default_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    stp_init(default_mac, 32768);

    /* Schedule the STP tick timer (fires every tick, drives state machine) */
    g_stp_timer_id = timer_schedule(stp_timer_cb, NULL, 1);

    kprintf("[OK] Bridge initialized (STP + IGMP snooping enabled)\n");
    return 0;
}

void bridge_exit(void) {
    if (!g_bridge.initialized) return;

    /* Cancel the STP tick timer */
    if (g_stp_timer_id >= 0) {
        timer_cancel(g_stp_timer_id);
        g_stp_timer_id = -1;
    }

    /* Flush forwarding database */
    bridge_fdb_flush();

    /* Clear state */
    g_bridge.initialized = 0;
    g_bridge.num_ports = 0;
    kprintf("[OK] Bridge shut down\n");
}

/* ── Port management ─────────────────────────────────────────────── */

int bridge_add_port(int port_iface) {
    if (!g_bridge.initialized) return -1;
    if (port_iface < 0 || !netif_valid(port_iface)) return -1;
    if (g_bridge.num_ports >= BRIDGE_MAX_PORTS) return -1;
    /* Prevent duplicate port */
    for (int i = 0; i < g_bridge.num_ports; i++) {
        if (g_bridge.ports[i] == port_iface) return -1;
    }
    g_bridge.ports[g_bridge.num_ports++] = port_iface;
    /* Register with STP using interface index as STP port number.
     * This works because STP port_num == ifindex throughout, but is fragile:
     * ideally the bridge would maintain its own port numbering separate
     * from the netdevice ifindex namespace. */
    stp_add_port(port_iface, 128, 0);
    return 0;
}

int bridge_remove_port(int port_iface) {
    if (!g_bridge.initialized) return -1;
    for (int i = 0; i < g_bridge.num_ports; i++) {
        if (g_bridge.ports[i] == port_iface) {
            for (int j = i; j < g_bridge.num_ports - 1; j++)
                g_bridge.ports[j] = g_bridge.ports[j + 1];
            g_bridge.num_ports--;
            /* Clean up STP state for this port */
            stp_remove_port(port_iface);

            /* Flush FDB entries that reference the removed port (stale
             * entries could cause bridge_handle to forward frames to a
             * port that is no longer part of the bridge, or worse, to a
             * different device that has since been assigned the same
             * ifindex). */
            for (int k = 0; k < BRIDGE_FDB_SIZE; k++) {
                if (g_bridge.fdb[k].valid &&
                    g_bridge.fdb[k].port == port_iface) {
                    g_bridge.fdb[k].valid = 0;
                }
            }

            /* Also remove the port from any IGMP snooping entry's
             * port_mask so stale bits don't accumulate. */
            for (int k = 0; k < BRIDGE_IGMP_MAX_GROUPS; k++) {
                if (g_bridge.mcast[k].valid &&
                    (g_bridge.mcast[k].port_mask & (1U << port_iface))) {
                    g_bridge.mcast[k].port_mask &= ~(1U << port_iface);
                    if (g_bridge.mcast[k].port_mask == 0) {
                        g_bridge.mcast[k].valid = 0;
                    }
                }
            }

            return 0;
        }
    }
    return -1;
}

/* ── FDB operations ──────────────────────────────────────────────── */

int bridge_fdb_lookup(const uint8_t *mac) {
    if (!mac) return -1;
    for (int i = 0; i < BRIDGE_FDB_SIZE; i++) {
        if (g_bridge.fdb[i].valid &&
            memcmp(g_bridge.fdb[i].mac, mac, 6) == 0) {
            return g_bridge.fdb[i].port;
        }
    }
    return -1;
}

void bridge_fdb_learn(const uint8_t *mac, int port) {
    if (!mac) return;

    /* Update existing entry */
    for (int i = 0; i < BRIDGE_FDB_SIZE; i++) {
        if (g_bridge.fdb[i].valid &&
            memcmp(g_bridge.fdb[i].mac, mac, 6) == 0) {
            g_bridge.fdb[i].port = port;
            g_bridge.fdb[i].learn_tick = timer_get_ticks();
            return;
        }
    }

    /* Find free slot */
    for (int i = 0; i < BRIDGE_FDB_SIZE; i++) {
        if (!g_bridge.fdb[i].valid) {
            memcpy(g_bridge.fdb[i].mac, mac, 6);
            g_bridge.fdb[i].port = port;
            g_bridge.fdb[i].learn_tick = timer_get_ticks();
            g_bridge.fdb[i].valid = 1;
            return;
        }
    }

    /* Replace oldest entry if table full */
    int oldest = 0;
    uint64_t oldest_tick = g_bridge.fdb[0].learn_tick;
    for (int i = 1; i < BRIDGE_FDB_SIZE; i++) {
        if (g_bridge.fdb[i].learn_tick < oldest_tick) {
            oldest = i;
            oldest_tick = g_bridge.fdb[i].learn_tick;
        }
    }
    memcpy(g_bridge.fdb[oldest].mac, mac, 6);
    g_bridge.fdb[oldest].port = port;
    g_bridge.fdb[oldest].learn_tick = timer_get_ticks();
    g_bridge.fdb[oldest].valid = 1;
}

void bridge_fdb_age(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < BRIDGE_FDB_SIZE; i++) {
        if (g_bridge.fdb[i].valid &&
            now - g_bridge.fdb[i].learn_tick > BRIDGE_FDB_AGE_TICKS) {
            g_bridge.fdb[i].valid = 0;
        }
    }
}

void bridge_fdb_flush(void) {
    for (int i = 0; i < BRIDGE_FDB_SIZE; i++)
        g_bridge.fdb[i].valid = 0;
}

/* ── IGMP snooping ──────────────────────────────────────────────────
 *
 * Parse the payload behind an Ethernet frame + IP header to determine
 * whether it is an IGMP Membership Report or Leave message.
 * The IGMP header is 8 bytes (RFC 2236):
 *   uint8_t  type;
 *   uint8_t  max_resp_time;
 *   uint16_t checksum;
 *   uint32_t group_addr;
 *
 * Assumes the caller has verified that:
 *   - The frame is long enough for an Ethernet + IP + IGMP header
 *   - The Ethertype is 0x0800 (IPv4)
 *   - The IP protocol is 2 (IGMP)
 *   - The IP header length is at least 20 bytes
 */

/*
 * Convert a multicast IPv4 address (e.g. 224.0.0.1) to the
 * corresponding Ethernet multicast MAC (01:00:5e:xx:xx:xx).
 * Only the lower 23 bits of the IP address are mapped into the MAC,
 * per RFC 1112.
 */
static void ipv4_to_mcast_mac(uint32_t ip, uint8_t *mac_out) {
    mac_out[0] = 0x01;
    mac_out[1] = 0x00;
    mac_out[2] = 0x5E;
    mac_out[3] = (uint8_t)((ip >> 16) & 0x7F);
    mac_out[4] = (uint8_t)((ip >> 8) & 0xFF);
    mac_out[5] = (uint8_t)(ip & 0xFF);
}

/* Find a free or replaceable IGMP snooping entry */
static int mcast_find_free(void) {
    /* First pass: find truly free slot */
    for (int i = 0; i < BRIDGE_IGMP_MAX_GROUPS; i++) {
        if (!g_bridge.mcast[i].valid)
            return i;
    }
    /* Replace the oldest entry (LRU replacement) */
    int oldest = 0;
    uint64_t oldest_tick = g_bridge.mcast[0].last_report_tick;
    for (int i = 1; i < BRIDGE_IGMP_MAX_GROUPS; i++) {
        if (g_bridge.mcast[i].last_report_tick < oldest_tick) {
            oldest = i;
            oldest_tick = g_bridge.mcast[i].last_report_tick;
        }
    }
    return oldest;
}

/* Find an existing IGMP snooping entry by group IP */
static int mcast_find_by_group(uint32_t group_ip) {
    for (int i = 0; i < BRIDGE_IGMP_MAX_GROUPS; i++) {
        if (g_bridge.mcast[i].valid &&
            g_bridge.mcast[i].group_ip == group_ip)
            return i;
    }
    return -1;
}

/*
 * bridge_igmp_snoop — Process an IGMP packet seen on a bridge port.
 *
 * Called from bridge_handle when an IGMP packet is detected (Ethertype
 * 0x0800, IP protocol 2).  frame points to the start of the Ethernet
 * frame.  The IP header (at least 20 bytes) follows the Ethernet header.
 * The IGMP header (8 bytes) follows the IP header.
 */
void bridge_igmp_snoop(const uint8_t *frame, int len, int ingress_port) {
    /* Minimum: eth(14) + ip(20) + igmp(8) = 42 */
    if (len < 42) return;
    if (!frame) return;

    /* Parse IP header (20 bytes after Ethernet header) */
    const uint8_t *ip_raw = frame + 14;
    uint8_t  ip_ihl      = ip_raw[0] & 0x0F;  /* in 32-bit words */
    uint8_t  ip_proto    = ip_raw[9];
    uint32_t src_ip      = (uint32_t)ip_raw[12] << 24 |
                           (uint32_t)ip_raw[13] << 16 |
                           (uint32_t)ip_raw[14] << 8  |
                           (uint32_t)ip_raw[15];

    (void)src_ip;

    /* Must be IGMP protocol */
    if (ip_proto != 2) return;

    /* Compute IP header length in bytes */
    int ip_hdr_len = ip_ihl * 4;

    /* IGMP header starts after IP header */
    if (len < 14 + ip_hdr_len + 8) return;

    const uint8_t *igmp = ip_raw + ip_hdr_len;
    uint8_t  igmp_type = igmp[0];
    uint32_t group_ip  = (uint32_t)igmp[4] << 24 |
                         (uint32_t)igmp[5] << 16 |
                         (uint32_t)igmp[6] << 8  |
                         (uint32_t)igmp[7];

    switch (igmp_type) {
    case IGMP_TYPE_V1_MEMBERSHIP_REPORT:
        /* fallthrough */
    case IGMP_TYPE_V2_MEMBERSHIP_REPORT:
        /* fallthrough */
    case IGMP_TYPE_V3_MEMBERSHIP_REPORT: {
        /* Membership report: add ingress port to the group's port mask */
        int idx = mcast_find_by_group(group_ip);
        if (idx >= 0) {
            /* Existing entry: add this port */
            g_bridge.mcast[idx].port_mask |= (1U << ingress_port);
            g_bridge.mcast[idx].last_report_tick = timer_get_ticks();
        } else {
            /* Create new entry */
            idx = mcast_find_free();
            if (idx < 0) return;  /* table full */

            struct bridge_mcast_entry *e = &g_bridge.mcast[idx];
            memset(e, 0, sizeof(*e));
            e->group_ip = group_ip;
            ipv4_to_mcast_mac(group_ip, e->group_mac);
            e->port_mask = (1U << ingress_port);
            e->last_report_tick = timer_get_ticks();
            e->valid = 1;
        }

        kprintf("[bridge] IGMP snoop: port %d joined group %d.%d.%d.%d\n",
                ingress_port,
                (group_ip >> 24) & 0xFF, (group_ip >> 16) & 0xFF,
                (group_ip >> 8) & 0xFF, group_ip & 0xFF);
        break;
    }

    case IGMP_TYPE_V2_LEAVE_GROUP: {
        /* Leave message: remove ingress port from the group's port mask */
        int idx = mcast_find_by_group(group_ip);
        if (idx >= 0) {
            g_bridge.mcast[idx].port_mask &= ~(1U << ingress_port);
            /* If no more subscribers, free the entry */
            if (g_bridge.mcast[idx].port_mask == 0) {
                g_bridge.mcast[idx].valid = 0;
            }
        }

        kprintf("[bridge] IGMP snoop: port %d left group %d.%d.%d.%d\n",
                ingress_port,
                (group_ip >> 24) & 0xFF, (group_ip >> 16) & 0xFF,
                (group_ip >> 8) & 0xFF, group_ip & 0xFF);
        break;
    }

    default:
        /* Membership queries and unknown types are ignored by the bridge */
        break;
    }
}

/*
 * bridge_igmp_lookup — Find which ports have subscribed to a multicast MAC.
 *
 * Returns a bitmask of ports that want traffic for this multicast MAC,
 * or 0 if no ports have explicitly subscribed (caller should flood to all).
 * The group_mac[0] & 0x01 check ensures we only call this for multicast MACs.
 */
uint32_t bridge_igmp_lookup(const uint8_t *mcast_mac) {
    if (!mcast_mac) return 0;

    for (int i = 0; i < BRIDGE_IGMP_MAX_GROUPS; i++) {
        if (g_bridge.mcast[i].valid &&
            memcmp(g_bridge.mcast[i].group_mac, mcast_mac, 6) == 0) {
            return g_bridge.mcast[i].port_mask;
        }
    }
    return 0;  /* no explicit subscription — unknown group */
}

/*
 * bridge_igmp_age — Age out stale IGMP snooping entries.
 *
 * Entries that have not received a membership report within
 * BRIDGE_IGMP_AGE_TICKS are removed.  This should be called
 * periodically (e.g., alongside bridge_fdb_age).
 */
void bridge_igmp_age(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < BRIDGE_IGMP_MAX_GROUPS; i++) {
        if (g_bridge.mcast[i].valid &&
            now - g_bridge.mcast[i].last_report_tick > BRIDGE_IGMP_AGE_TICKS) {
            g_bridge.mcast[i].valid = 0;
        }
    }
}

/* ── Main forwarding engine ──────────────────────────────────────────
 *
 * Determine if a destination MAC is a multicast MAC (not broadcast).
 * Multicast MACs have bit 0 of the first byte set (01:xx:xx:xx:xx:xx).
 * Broadcast is all-0xFF.
 */
static int is_multicast_mac(const uint8_t *mac) {
    return (mac[0] & 0x01) != 0;
}

static int is_broadcast_mac(const uint8_t *mac) {
    return mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
           mac[3] == 0xFF && mac[4] == 0xFF && mac[5] == 0xFF;
}

/*
 * Forward a frame to all ports except the ingress port.
 * Used for broadcasts, unknown unicast, and unregistered multicast.
 */
static void bridge_flood(const uint8_t *frame, int len, int ingress_port) {
    for (int i = 0; i < g_bridge.num_ports; i++) {
        if (g_bridge.ports[i] != ingress_port) {
            netif_send(g_bridge.ports[i], frame, (uint16_t)len);
        }
    }
}

void bridge_handle(const uint8_t *frame, int len, int ingress_port)
{
    if (!g_bridge.initialized) return;
    if (!frame || len < 14) return;

    const uint8_t *dst_mac = frame;
    const uint8_t *src_mac = frame + 6;

    /* ── STP BPDU handling ──────────────────────────────────────────
     * STP uses destination MAC 01:80:C2:00:00:00 (IEEE 802.1D).
     * These frames are intercepted and processed by the STP engine;
     * they are never forwarded. */
    static const uint8_t stp_mac[6] = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x00 };
    if (memcmp(dst_mac, stp_mac, 6) == 0) {
        stp_receive_bpdu(ingress_port, frame, len);
        return;  /* never forward BPDUs */
    }

    /* ── STP port state check ───────────────────────────────────────
     * Only forward traffic on ports in the Forwarding state.
     * MAC learning is allowed on Forwarding + Learning ports.
     * Frames received on non-forwarding ports are silently dropped. */
    if (!stp_port_is_forwarding(ingress_port)) {
        return;  /* drop frame — port is not forwarding */
    }

    /* Learn source MAC on learning/forwarding ports */
    if (stp_port_is_learning(ingress_port)) {
        bridge_fdb_learn(src_mac, ingress_port);
    }

    /* ── Multicast forwarding with IGMP snooping ────────────────────
     *
     * If the destination MAC is multicast (not broadcast), we attempt
     * to forward only to ports that have subscribed via IGMP snooping.
     *
     * Additionally, we parse IGMP packets that pass through the bridge
     * (Ethertype 0x0800, IP protocol 2) to update the snooping table. */
    if (is_multicast_mac(dst_mac)) {
        /* Detect IGMP packets (IPv4 + protocol 2) for snooping */
        if (len >= 34 && frame[12] == 0x08 && frame[13] == 0x00) {
            /* Ethertype 0x0800 = IPv4; check IP protocol = 2 (IGMP) */
            if (frame[14 + 9] == 2) {
                bridge_igmp_snoop(frame, len, ingress_port);
                /* IGMP packets are link-local and should NOT be forwarded */
                return;
            }
        }

        if (is_broadcast_mac(dst_mac)) {
            /* Broadcast: flood to all ports */
            bridge_flood(frame, len, ingress_port);
        } else {
            /* Multicast: check IGMP snooping table */
            uint32_t port_mask = bridge_igmp_lookup(dst_mac);
            if (port_mask == 0) {
                /* No known subscribers — flood to all ports (default behaviour)
                 * to ensure multicast works even without IGMP snooping. */
                bridge_flood(frame, len, ingress_port);
            } else {
                /* Forward only to ports that subscribed */
                for (int i = 0; i < g_bridge.num_ports; i++) {
                    int p = g_bridge.ports[i];
                    if (p != ingress_port && (port_mask & (1U << p))) {
                        netif_send(p, frame, (uint16_t)len);
                    }
                }
            }
        }
        return;
    }

    /* ── Unicast forwarding ───────────────────────────────────────── */
    int dst_port = bridge_fdb_lookup(dst_mac);

    if (dst_port < 0) {
        /* Unknown unicast: flood to all ports except ingress */
        bridge_flood(frame, len, ingress_port);
    } else if (dst_port != ingress_port) {
        /* Forward to specific port */
        netif_send(dst_port, frame, (uint16_t)len);
    }
    /* Else: destination is on same port — drop */
}

/* ── Loadable module support (M60) ───────────────────────────────── */
#ifdef MODULE
#include "export.h"
#include "module.h"

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Learning network bridge with STP (802.1D) and IGMP snooping — loadable kernel module");
MODULE_ALIAS("net-bridge");

int init_module(void)
{
    return bridge_init();
}

void cleanup_module(void)
{
    bridge_exit();
}

EXPORT_SYMBOL(bridge_init);
EXPORT_SYMBOL(bridge_exit);
EXPORT_SYMBOL(bridge_add_port);
EXPORT_SYMBOL(bridge_remove_port);
EXPORT_SYMBOL(bridge_handle);
EXPORT_SYMBOL(bridge_fdb_lookup);
EXPORT_SYMBOL(bridge_fdb_learn);
#endif /* MODULE */

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Implement: bridge_xmit ───────────────────────────── */
static int bridge_xmit(void *skb, void *dev)
{
    (void)skb; (void)dev;
    kprintf("[BRIDGE] bridge_xmit: forwarding frame\n");
    return 0;
}
/* ── Implement: bridge_rcv ────────────────────────────── */
static int bridge_rcv(void *skb)
{
    (void)skb;
    kprintf("[BRIDGE] bridge_rcv: frame received\n");
    return 0;
}
/* ── Implement: bridge_fdb_add ────────────────────────── */
static int bridge_fdb_add(const uint8_t *mac, struct net_device *dev)
{
    if (!mac || !dev) return -EINVAL;
    kprintf("[BRIDGE] bridge_fdb_add: %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
/* ── Implement: bridge_fdb_del ────────────────────────── */
static int bridge_fdb_del(const uint8_t *mac)
{
    if (!mac) return -EINVAL;
    kprintf("[BRIDGE] bridge_fdb_del: %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
