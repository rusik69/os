/* stp.c — 802.1D-1998 Spanning Tree Protocol implementation
 *
 * Implements the classic STP (not RSTP/MSTP) for loop-free bridge
 * topology.  Integrates with the bridge's bridge_handle() via
 * stp_receive_bpdu() and with the timer subsystem via stp_tick().
 *
 * Reference: IEEE 802.1D-1998, clauses 8 and 9. */

#define KERNEL_INTERNAL
#include "stp.h"
#include "bridge.h"
#include "netdevice.h"
#include "net.h"         /* ETH_TYPE_IP */
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "heap.h"

/* ── Bridge ID helpers ────────────────────────────────────────────── */

/* Build a bridge ID from a 2-byte priority (big-endian) and 6-byte MAC.
 * Bridge ID layout: priority (2 bytes, big-endian) + MAC (6 bytes). */
static uint64_t make_bridge_id(int priority, const uint8_t *mac) {
    uint64_t id = 0;
    id |= ((uint64_t)((priority >> 8) & 0xFF)) << 56;
    id |= ((uint64_t)(priority & 0xFF)) << 48;
    id |= ((uint64_t)mac[0]) << 40;
    id |= ((uint64_t)mac[1]) << 32;
    id |= ((uint64_t)mac[2]) << 24;
    id |= ((uint64_t)mac[3]) << 16;
    id |= ((uint64_t)mac[4]) << 8;
    id |= ((uint64_t)mac[5]);
    return id;
}

/* ── STP global state ────────────────────────────────────────────── */

static struct stp_bridge g_stp;
static int g_initialized = 0;

/* STP multicast address: 01:80:C2:00:00:00 (IEEE 802.1D, clause 7.12) */
static const uint8_t stp_dmac[6] = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x00 };

/* ── Conversion helpers ──────────────────────────────────────────── */

/* Convert seconds to timer ticks */
static inline uint64_t sec_to_ticks(int sec) {
    return (uint64_t)sec * TIMER_FREQ;
}

/* Convert seconds to units of 1/256 second (BPDU format) */
static inline uint16_t sec_to_bpdu_time(int sec) {
    return (uint16_t)(sec * 256);
}

/* Convert BPDU time (1/256 sec units) to timer ticks */
static inline uint64_t bpdu_to_ticks(uint16_t bpdu_time) {
    return ((uint64_t)bpdu_time * TIMER_FREQ) / 256;
}

/* ── Path cost computation ──────────────────────────────────────────
 * 802.1D-1998 recommended path cost values (Table 8-1). */

static uint32_t compute_path_cost(int port_speed_mbps) {
    if (port_speed_mbps <= 0)   return 100;   /* unknown / default */
    if (port_speed_mbps >= 1000) return 4;    /* 1 Gbps */
    if (port_speed_mbps >= 100)  return 19;   /* 100 Mbps */
    if (port_speed_mbps >= 10)   return 100;  /* 10 Mbps */
    return 1000;                               /* 1 Mbps or less */
}

/* ── BPDU helpers ─────────────────────────────────────────────────── */

/* Build a raw Ethernet frame containing a Configuration BPDU.
 * Returns the total length of the frame (Ethernet header + BPDU).
 * Caller must provide a buffer of at least 64 bytes. */
static int build_config_bpdu(uint8_t *buf, int port_num) {
    uint8_t *eth = buf;

    /* Ethernet header: dst = STP multicast, src = port MAC (unknown),
     * length = 38 bytes (BPDU is LLC-encoded with DSAP/SSAP = 0x42) */
    memcpy(eth, stp_dmac, 6);
    /* Source MAC: for simplicity, use the bridge's MAC from the ID.
     * On a real bridge each port has its own MAC, but for a 1-MAC bridge
     * the same MAC is used.  We extract it from the bridge ID. */
    eth[6]  = (uint8_t)(g_stp.bridge_id >> 40);
    eth[7]  = (uint8_t)(g_stp.bridge_id >> 32);
    eth[8]  = (uint8_t)(g_stp.bridge_id >> 24);
    eth[9]  = (uint8_t)(g_stp.bridge_id >> 16);
    eth[10] = (uint8_t)(g_stp.bridge_id >> 8);
    eth[11] = (uint8_t)(g_stp.bridge_id);

    /* LLC encapsulation: DSAP=0x42, SSAP=0x42, CTRL=0x03 (UI) */
    eth[12] = 0x42;  /* DSAP = STP (IEEE 802.1D) */
    eth[13] = 0x42;  /* SSAP */
    eth[14] = 0x03;  /* Control = Unnumbered Information */
    /* Length field for LLC frames: 38 bytes after the length field.
     * Actually for 802.3, the length field is at offset 12; with LLC
     * it goes DSAP/SSAP/CTRL.  But the simplest approach: treat the
     * Ethernet header as 14 bytes, then write the LLC+SNAP for STP.
     *
     * We'll use a 22-byte LLC+SNAP header + 35-byte BPDU.
     * Standard 802.3/Ethernet encapsulation varies.  For simplicity,
     * we'll just write the BPDU data directly after a 14-byte header
     * with EtherType 0x0026? No, STP uses LLC not EtherType.
     *
     * Common practice (Linux bridge): use 802.3 header with length,
     * then LLC: DSAP=0x42, SSAP=0x42, CTRL=0x03, then BPDU.
     * The 802.3 length field = total after length - 14 = 35+3 = 38.
     */

    /* Minimum Ethernet frame length is 64 bytes including CRC.
     * Our frame: 14 (header) + 3 (LLC) + 35 (BPDU) = 52 bytes.
     * The NIC will pad+CRC automatically (or we need manually). */

    /* Write LLC header at offset 14 */
    buf[14] = 0x42;  /* DSAP */
    buf[15] = 0x42;  /* SSAP */
    buf[16] = 0x03;  /* Control = UI */

    /* Write the 802.3 length field at offset 12 (instead of EtherType):
     * Length = total after length field = 3 (LLC) + 35 (BPDU) = 38 */
    eth[12] = 0x00;
    eth[13] = 38;    /* 3 + 35 = 38 bytes after length field */

    /* BPDU starts at offset 17 (14 + 3) */
    struct stp_bpdu_config *bpdu = (struct stp_bpdu_config *)(buf + 17);
    memset(bpdu, 0, sizeof(*bpdu));
    bpdu->protocol_id      = __builtin_bswap16(STP_PROTOCOL_ID);
    bpdu->protocol_version = STP_PROTOCOL_VERSION;
    bpdu->bpdu_type        = STP_BPDU_TYPE_CONFIG;

    /* Set flags */
    uint8_t flags = 0;
    if (g_stp.topology_change) flags |= STP_FLAG_TC;
    /* We don't set TCA (Topology Change Acknowledgment) in root's BPDUs */
    bpdu->flags = flags;

    bpdu->root_id         = g_stp.root_id;
    bpdu->root_path_cost  = g_stp.root_path_cost;
    bpdu->bridge_id       = g_stp.bridge_id;
    bpdu->port_id         = __builtin_bswap16(
        g_stp.ports[port_num].port_id);
    bpdu->message_age     = __builtin_bswap16(0);   /* always 0 for root */
    bpdu->max_age         = __builtin_bswap16((uint16_t)g_stp.max_age);
    bpdu->hello_time      = __builtin_bswap16((uint16_t)g_stp.hello_time);
    bpdu->forward_delay   = __builtin_bswap16((uint16_t)g_stp.forward_delay);

    /* Total size: 14 (eth) + 3 (LLC) + 35 (BPDU) = 52 */
    return 52;
}

/* ── BPDU transmission ────────────────────────────────────────────── */

void stp_send_config(int port_num) {
    if (!g_initialized) return;
    if (port_num < 0 || port_num >= g_stp.num_ports) return;
    if (!g_stp.ports[port_num].enabled) return;

    uint8_t frame[64];
    int len = build_config_bpdu(frame, port_num);

    /* Find the bridge port netif index.  The STP port numbers correspond
     * to bridge ports (0, 1, 2...).  We need to translate to a netif index.
     * We call netif_send directly on the port's interface.
     *
     * Since the bridge manages ports by ifindex, we need a mapping from
     * STP port number to netif ifindex.  We'll use the bridge port table.
     * For now, we simply attempt to send on the port's (0-indexed) netif.
     * This assumes port_num corresponds to a valid interface index. */

    /* We need access to the bridge's port tables.  The bridge expose them
     * via internal functions?  For simplicity, we look up the netif directly.
     * Bridge ports are stored as ifindex values.  We need to know which
     * ifindex corresponds to our STP port_num.  We'll assume the bridge
     * assigns ports contiguously starting from the first registered port.
     * The bridge_add_port() function stores ports in order, and the first
     * added port is at index 0 in the bridge port list.
     *
     * Problem: we don't have access to g_bridge from here.  We'll use
     * a simple approach: the port_num is the 0-based interface index
     * of the bridge port.  This is recorded when stp_add_port() is called. */

    /* For now, send on port_num ifindex (bridge port mapping). */
    netif_send(port_num, frame, (uint16_t)len);
}

void stp_send_tcn(void) {
    if (!g_initialized) return;

    /* Build a TCN BPDU: protocol_id(2) + ver(1) + type(1) + padding */
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    /* Ethernet: STP multicast addr */
    memcpy(buf, stp_dmac, 6);
    buf[6]  = (uint8_t)(g_stp.bridge_id >> 40);
    buf[7]  = (uint8_t)(g_stp.bridge_id >> 32);
    buf[8]  = (uint8_t)(g_stp.bridge_id >> 24);
    buf[9]  = (uint8_t)(g_stp.bridge_id >> 16);
    buf[10] = (uint8_t)(g_stp.bridge_id >> 8);
    buf[11] = (uint8_t)(g_stp.bridge_id);

    /* 802.3 length = 7 bytes (LLC + BPDU) */
    buf[12] = 0x00;
    buf[13] = 7;    /* 3 (LLC) + 4 (TCN) */

    /* LLC: DSAP=0x42, SSAP=0x42, CTRL=0x03 */
    buf[14] = 0x42;
    buf[15] = 0x42;
    buf[16] = 0x03;

    /* TCN BPDU */
    struct stp_bpdu_tcn *tcn = (struct stp_bpdu_tcn *)(buf + 17);
    tcn->protocol_id      = __builtin_bswap16(STP_PROTOCOL_ID);
    tcn->protocol_version = STP_PROTOCOL_VERSION;
    tcn->bpdu_type        = STP_BPDU_TYPE_TCN;

    int len = 21; /* 14 + 3 + 4 = 21 bytes */

    /* Send on the root port (the one toward the root bridge) */
    if (g_stp.root_port >= 0 && g_stp.root_port < g_stp.num_ports) {
        netif_send(g_stp.root_port, buf, (uint16_t)len);
        kprintf("[stp] Sent TCN on port %d\n", g_stp.root_port);
    } else {
        /* As root bridge, there is no root port; TCN is not sent by root */
        kprintf("[stp] We are root — not sending TCN\n");
    }
}

/* ── BPDU reception ───────────────────────────────────────────────── */

void stp_receive_bpdu(int port_num, const uint8_t *frame, int len) {
    if (!g_initialized) return;
    if (port_num < 0 || port_num >= g_stp.num_ports) return;
    if (!frame || len < 21) return;  /* need at least eth(14)+LLC(3)+TCN(4) */

    struct stp_port *port = &g_stp.ports[port_num];
    if (!port->enabled) return;

    /* Skip Ethernet header (14 bytes) and LLC header (3 bytes) */
    const uint8_t *llc = frame + 14;
    /* Check LLC: DSAP=0x42, SSAP=0x42, CTRL=0x03 (UI) */
    if (llc[0] != 0x42 || llc[1] != 0x42 || llc[2] != 0x03)
        return;

    const uint8_t *bpdu_raw = frame + 17;
    int bpdu_len = len - 17;
    if (bpdu_len < 4) return;

    uint8_t bpdu_type = bpdu_raw[2]; /* offset within BPDU */

    if (bpdu_type == STP_BPDU_TYPE_CONFIG) {
        /* Configuration BPDU */
        if (bpdu_len < (int)sizeof(struct stp_bpdu_config)) return;
        const struct stp_bpdu_config *bpdu =
            (const struct stp_bpdu_config *)(bpdu_raw);

        uint64_t msg_root_id         = bpdu->root_id;
        uint32_t msg_root_path_cost  = __builtin_bswap32(bpdu->root_path_cost);
        uint64_t msg_bridge_id       = bpdu->bridge_id;
        uint16_t msg_port_id         = __builtin_bswap16(bpdu->port_id);
        uint16_t msg_message_age     = __builtin_bswap16(bpdu->message_age);

        (void)msg_port_id;
        (void)msg_message_age;

        /* ── Root bridge election logic ──────────────────────────────
         * The best BPDU is the one with:
         * 1. Lowest root ID
         * 2. Lowest root path cost
         * 3. Lowest bridge ID
         * 4. Lowest port ID
         *
         * We compare the received BPDU against what this port thinks
         * the best information is. */

        int become_root = 0;
        int become_designated = 0;

        /* Compute the path cost from here to the received root */
        uint32_t our_cost = msg_root_path_cost + port->path_cost;

        /* Is the received BPDU better than our current root info? */
        int better_root = 0;
        if (msg_root_id < g_stp.root_id)
            better_root = 1;
        else if (msg_root_id == g_stp.root_id &&
                 our_cost < g_stp.root_path_cost)
            better_root = 1;
        else if (msg_root_id == g_stp.root_id &&
                 our_cost == g_stp.root_path_cost &&
                 msg_bridge_id < g_stp.bridge_id)
            better_root = 1;
        else if (msg_root_id == g_stp.root_id &&
                 our_cost == g_stp.root_path_cost &&
                 msg_bridge_id == g_stp.bridge_id &&
                 msg_port_id < port->port_id)
            better_root = 1;

        if (better_root) {
            /* Update our view of the root */
            kprintf("[stp] Port %d: better root info received (root=%llx, cost=%lu)\n",
                    port_num, (unsigned long long)msg_root_id,
                    (unsigned long)our_cost);

            g_stp.root_id = msg_root_id;
            g_stp.root_path_cost = our_cost;
            g_stp.root_port = port_num;

            /* Update designated info for this port */
            port->designated_bridge = msg_bridge_id;
            port->designated_cost = msg_root_path_cost;
            port->designated_port = msg_port_id;

            become_root = 0;
        }

        /* Check if this message on a designated port tells us we are not
         * the designated bridge for that port. */
        if (msg_bridge_id < g_stp.bridge_id) {
            /* Another bridge has a better ID — if it claims to be
             * designated for our port, we must become alternate. */
            /* Simplified: if the received BPDU is better than our
             * designated info for this port, we need to react. */
            uint64_t our_desig = port->designated_bridge;
            if (our_desig == 0 || msg_bridge_id < our_desig ||
                (msg_bridge_id == our_desig &&
                 msg_port_id < port->designated_port)) {
                /* Received info is better → we're not designated */
                port->designated_bridge = msg_bridge_id;
                port->designated_cost = msg_root_path_cost;
                port->designated_port = msg_port_id;

                /* Transition this port to Blocking if it was Designated */
                if (port->role == STP_ROLE_DESIGNATED) {
                    port->role = STP_ROLE_ALTERNATE;
                    port->state = STP_PORT_BLOCKING;
                    port->delay_timer = 0;  /* stop transition timer */
                    kprintf("[stp] Port %d: became alternate (blocking)\n", port_num);
                }
            }
        } else if (msg_bridge_id == g_stp.bridge_id) {
            /* Received our own BPDU — loop detected.  Another bridge is
             * sending BPDUs that claim we are the root.  This port should
             * transition to Blocking. */
            if (port->role == STP_ROLE_DESIGNATED) {
                port->role = STP_ROLE_ALTERNATE;
                port->state = STP_PORT_BLOCKING;
                port->delay_timer = 0;
                kprintf("[stp] Port %d: loop detected (blocking)\n", port_num);
                become_root = 0;
            }
        } else {
            /* msg_bridge_id > our bridge_id — we are better bridge.
             * This port is designated.  Ensure it transitions. */
            if (port->role != STP_ROLE_DESIGNATED) {
                port->role = STP_ROLE_DESIGNATED;
                port->state = STP_PORT_LISTENING;
                port->delay_timer = g_stp.forward_delay_ticks;
                port->fd_while = 0;
                kprintf("[stp] Port %d: became designated (listening)\n", port_num);
            }
        }

        (void)become_root;
        (void)become_designated;

    } else if (bpdu_type == STP_BPDU_TYPE_TCN) {
        /* Topology Change Notification BPDU */
        kprintf("[stp] TCN received on port %d\n", port_num);

        /* If we are the root bridge, set TC flag and start timer */
        if (g_stp.root_port < 0) {
            /* We are root */
            g_stp.topology_change = 1;
            g_stp.tc_timer = g_stp.forward_delay_ticks + g_stp.max_age_ticks;
            kprintf("[stp] Root bridge — setting TC flag\n");

            /* Send TCA back on the port where TCN was received.
             * TCA is sent by root by setting FLAG_TCA in its next
             * Config BPDU on that port.  We'll set a flag for that
             * port to acknowledge. */
        } else {
            /* Not root: forward TCN to root port */
            stp_send_tcn();
        }
    }
}

/* ── Port state machine ─────────────────────────────────────────────
 * 802.1D-1998 clause 8.4:
 *
 *   Disabled → Blocking (if enabled)
 *   Blocking → Listening (after max_age + forward_delay? or on root/desig)
 *   Listening → Learning (after forward_delay)
 *   Learning → Forwarding (after forward_delay)
 *   Any      → Disabled/Blocking (on change of role)
 *
 * For simplicity: when a port becomes Designated or Root, go
 * Listening → Learning → Forwarding with forward_delay each step.
 */

static void stp_port_state_machine(void) {
    for (int i = 0; i < g_stp.num_ports; i++) {
        struct stp_port *p = &g_stp.ports[i];
        if (!p->enabled) {
            if (p->state != STP_PORT_DISABLED) {
                p->state = STP_PORT_DISABLED;
                p->delay_timer = 0;
            }
            continue;
        }

        switch (p->state) {
        case STP_PORT_DISABLED:
            /* Enabled port starts in Blocking */
            p->state = STP_PORT_BLOCKING;
            p->role = STP_ROLE_ALTERNATE;
            p->delay_timer = 0;
            break;

        case STP_PORT_BLOCKING:
            /* Transition to Listening when:
             * - We are root and this is not the root port (all desig), OR
             * - This port is designated or root port
             * Simplified: after max_age ticks in blocking, start listening */
            if (p->role == STP_ROLE_DESIGNATED ||
                p->role == STP_ROLE_ROOT_PORT ||
                p->role == STP_ROLE_UNKNOWN) {
                /* Set role if unknown */
                if (p->role == STP_ROLE_UNKNOWN) {
                    if (g_stp.root_port < 0) {
                        /* We are root → all ports designated */
                        p->role = STP_ROLE_DESIGNATED;
                    } else if (i == g_stp.root_port) {
                        p->role = STP_ROLE_ROOT_PORT;
                    } else {
                        p->role = STP_ROLE_DESIGNATED;
                    }
                    kprintf("[stp] Port %d: assigned role %d\n", i, p->role);
                }
                p->state = STP_PORT_LISTENING;
                p->delay_timer = g_stp.forward_delay_ticks;
                p->fd_while = 0;
                kprintf("[stp] Port %d: blocking → listening\n", i);
            }
            break;

        case STP_PORT_LISTENING:
            /* After forward_delay, move to Learning */
            if (p->delay_timer == 0) {
                p->state = STP_PORT_LEARNING;
                p->delay_timer = g_stp.forward_delay_ticks;
                p->fd_while = 0;
                kprintf("[stp] Port %d: listening → learning\n", i);
            }
            break;

        case STP_PORT_LEARNING:
            /* After forward_delay, move to Forwarding */
            if (p->delay_timer == 0) {
                p->state = STP_PORT_FORWARDING;
                p->delay_timer = 0;
                kprintf("[stp] Port %d: learning → forwarding\n", i);

                /* Topology change: root sets TC flag */
                if (g_stp.root_port < 0) {
                    g_stp.topology_change = 1;
                    g_stp.tc_timer = g_stp.forward_delay_ticks + g_stp.max_age_ticks;
                } else {
                    /* Non-root: send TCN toward root */
                    stp_send_tcn();
                }
            }
            break;

        case STP_PORT_FORWARDING:
            /* Forwarding state — stable, no transition needed unless role changes */
            break;
        default:
            break;
        }
    }
}

/* ── Periodic tick ────────────────────────────────────────────────── */

void stp_tick(void) {
    if (!g_initialized) return;

    uint64_t now = timer_get_ticks();

    /* 1. Run down timers (forward delay per port) */
    for (int i = 0; i < g_stp.num_ports; i++) {
        struct stp_port *p = &g_stp.ports[i];
        if (p->enabled && p->delay_timer > 0) {
            p->delay_timer--;
        }
    }

    /* 2. Topology change timer */
    if (g_stp.topology_change && g_stp.tc_timer > 0) {
        g_stp.tc_timer--;
        if (g_stp.tc_timer == 0) {
            g_stp.topology_change = 0;
            kprintf("[stp] Topology change ended\n");
        }
    }

    /* 3. State machine transitions */
    stp_port_state_machine();

    /* 4. Send hello BPDUs (every hello_time) on designated ports */
    if (g_stp.root_port < 0) {
        /* We are the root — send Config BPDUs on all designated ports */
        if (now - g_stp.last_hello_tick >= g_stp.hello_time_ticks) {
            for (int i = 0; i < g_stp.num_ports; i++) {
                if (g_stp.ports[i].enabled &&
                    g_stp.ports[i].role == STP_ROLE_DESIGNATED) {
                    stp_send_config(i);
                }
            }
            g_stp.last_hello_tick = now;
        }
    } else {
        /* Non-root: we might also send Config BPDUs on designated ports */
        if (now - g_stp.last_hello_tick >= g_stp.hello_time_ticks) {
            for (int i = 0; i < g_stp.num_ports; i++) {
                if (g_stp.ports[i].enabled &&
                    g_stp.ports[i].role == STP_ROLE_DESIGNATED) {
                    stp_send_config(i);
                }
            }
            g_stp.last_hello_tick = now;
        }
    }
}

/* ── Initialisation and port management ──────────────────────────── */

void stp_init(uint8_t *bridge_mac, int bridge_id_priority) {
    if (g_initialized) return;

    memset(&g_stp, 0, sizeof(g_stp));

    if (bridge_id_priority <= 0)
        bridge_id_priority = STP_BRIDGE_PRIORITY_DEF;
    /* Ensure priority is a multiple of 4096 (per 802.1t) but accept any */
    bridge_id_priority &= 0xFFFF;

    if (bridge_mac) {
        g_stp.bridge_id = make_bridge_id(bridge_id_priority, bridge_mac);
    } else {
        /* Default MAC if none provided */
        uint8_t default_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
        g_stp.bridge_id = make_bridge_id(bridge_id_priority, default_mac);
    }

    /* Initially we are the root bridge */
    g_stp.root_id        = g_stp.bridge_id;
    g_stp.designated_root = g_stp.root_id;
    g_stp.root_path_cost = 0;
    g_stp.root_port      = -1;   /* we are root */
    g_stp.topology_change = 0;
    g_stp.tc_timer       = 0;
    g_stp.last_hello_tick = 0;

    /* Default timer values */
    g_stp.hello_time        = sec_to_bpdu_time(STP_HELLO_TIME_DEF);
    g_stp.max_age           = sec_to_bpdu_time(STP_MAX_AGE_DEF);
    g_stp.forward_delay     = sec_to_bpdu_time(STP_FORWARD_DELAY_DEF);
    g_stp.hello_time_ticks  = sec_to_ticks(STP_HELLO_TIME_DEF);
    g_stp.max_age_ticks     = sec_to_ticks(STP_MAX_AGE_DEF);
    g_stp.forward_delay_ticks = sec_to_ticks(STP_FORWARD_DELAY_DEF);

    g_stp.num_ports  = 0;
    g_stp.initialized = 1;

    kprintf("[OK] STP initialized (bridge_id=%llx, prio=%d)\n",
            (unsigned long long)g_stp.bridge_id, bridge_id_priority);
}

void stp_add_port(int port_num, int port_priority, uint32_t path_cost) {
    if (!g_initialized) return;
    if (port_num < 0 || port_num >= STP_MAX_PORTS) return;
    if (port_num >= g_stp.num_ports)
        g_stp.num_ports = port_num + 1;

    struct stp_port *p = &g_stp.ports[port_num];
    memset(p, 0, sizeof(*p));

    p->enabled = 1;
    p->state   = STP_PORT_DISABLED;   /* will transition to Blocking */
    p->role    = STP_ROLE_UNKNOWN;

    if (port_priority <= 0)
        port_priority = 128;   /* default port priority */
    p->port_id = (uint16_t)(((port_priority & 0xFF) << 8) | (port_num & 0xFF));

    if (path_cost == 0)
        path_cost = compute_path_cost(100);   /* assume 100 Mbps default */
    p->path_cost = path_cost;

    /* Initially we are root, so all ports are designated */
    p->designated_bridge = g_stp.bridge_id;
    p->designated_cost   = 0;
    p->designated_port   = p->port_id;

    kprintf("[stp] Added port %d (id=0x%04x, cost=%lu)\n",
            port_num, p->port_id, (unsigned long)path_cost);
}

void stp_remove_port(int port_num) {
    if (!g_initialized) return;
    if (port_num < 0 || port_num >= g_stp.num_ports) return;

    struct stp_port *p = &g_stp.ports[port_num];
    memset(p, 0, sizeof(*p));
    kprintf("[stp] Removed port %d\n", port_num);
}

/* ── Query API ────────────────────────────────────────────────────── */

int stp_port_state(int port_num) {
    if (!g_initialized) return STP_PORT_DISABLED;
    if (port_num < 0 || port_num >= g_stp.num_ports)
        return STP_PORT_DISABLED;
    return g_stp.ports[port_num].state;
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Implement: stp_xmit ────────────────── */
int stp_xmit(void *skb, int port_num)
{
    if (!skb || port_num < 0) {
        kprintf("[stp] stp_xmit: invalid parameter (skb=%p port=%d)\n", skb, port_num);
        return -EINVAL;
    }
    kprintf("[stp] stp_xmit: skb=%p port=%d (stub)\n", skb, port_num);
    return -EOPNOTSUPP;
}
/* ── Implement: stp_rcv ────────────────── */
int stp_rcv(void *skb, int port_num)
{
    if (!skb || port_num < 0) {
        kprintf("[stp] stp_rcv: invalid parameter (skb=%p port=%d)\n", skb, port_num);
        return -EINVAL;
    }
    kprintf("[stp] stp_rcv: skb=%p port=%d (stub)\n", skb, port_num);
    return -EOPNOTSUPP;
}
/* ── Stub: stp_become_root ─────────────────────────── */
void stp_become_root(void)
{
    kprintf("[STP] stp_become_root: not yet implemented\n");
}
/* ── Stub: stp_become_designated ───────────────────── */
void stp_become_designated(int port_num)
{
    (void)port_num;
    kprintf("[STP] stp_become_designated: not yet implemented\n");
}
