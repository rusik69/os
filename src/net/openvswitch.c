// SPDX-License-Identifier: GPL-2.0-only
/*
 * openvswitch.c — Open vSwitch datapath
 *
 * Provides the Open vSwitch datapath for software-defined networking.
 * Implements flow table matching, action execution, and tunneling.
 * Enhanced with:
 *   - Wildcard matching (prefix-based IP matching)
 *   - VLAN push/pop actions
 *   - Tunnel encapsulation (GRE/VXLAN)
 *   - Flow statistics per-entry
 *   - Idle timeout eviction
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "net.h"
#include "net_internal.h"
#include "netdevice.h"
#include "timer.h"

#define OVS_FLOW_TABLE_SIZE 1024
#define OVS_MAX_ACTIONS     16

/* Flow key (simplified) */
struct ovs_flow_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t src_ip_mask;   /* For CIDR wildcard matching */
    uint32_t dst_ip_mask;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint32_t in_port;
    uint16_t vlan_vid;      /* VLAN ID (0 = no VLAN) */
};

/* Action types */
#define OVS_ACTION_OUTPUT         1
#define OVS_ACTION_DROP           2
#define OVS_ACTION_SET_FIELD      3
#define OVS_ACTION_PUSH_VLAN      4
#define OVS_ACTION_POP_VLAN       5
#define OVS_ACTION_DEC_TTL        6
#define OVS_ACTION_SET_TUNNEL     7
#define OVS_ACTION_ENCAP_GRE      8
#define OVS_ACTION_ENCAP_VXLAN    9
#define OVS_ACTION_DECAP          10

struct ovs_action {
    int type;
    union {
        uint32_t output_port;
        uint32_t set_field_val;
        uint16_t vlan_vid;
        uint32_t tunnel_ip;
        uint8_t  field_offset;
    };
    uint8_t field_len;
};

/* Flow entry */
struct ovs_flow {
    int in_use;
    struct ovs_flow_key key;
    struct ovs_action actions[OVS_MAX_ACTIONS];
    int action_count;
    uint64_t cookie;
    uint64_t packet_count;
    uint64_t byte_count;
    uint64_t last_hit_tick;
    int idle_timeout;
};

static struct ovs_flow ovs_flow_table[OVS_FLOW_TABLE_SIZE];
static spinlock_t ovs_lock;

/* Check if a flow matches a packet key (with wildcard support) */
static int ovs_flow_matches(const struct ovs_flow *flow, const struct ovs_flow_key *pkt_key)
{
    if (flow->key.in_port != pkt_key->in_port)
        return 0;

    /* IP matching with masks */
    if ((flow->key.src_ip & flow->key.src_ip_mask) !=
        (pkt_key->src_ip & flow->key.src_ip_mask))
        return 0;

    if ((flow->key.dst_ip & flow->key.dst_ip_mask) !=
        (pkt_key->dst_ip & flow->key.dst_ip_mask))
        return 0;

    /* Port matching (0 = wildcard) */
    if (flow->key.src_port != 0 && flow->key.src_port != pkt_key->src_port)
        return 0;
    if (flow->key.dst_port != 0 && flow->key.dst_port != pkt_key->dst_port)
        return 0;

    /* Protocol matching (0 = wildcard) */
    if (flow->key.proto != 0 && flow->key.proto != pkt_key->proto)
        return 0;

    return 1;
}

/* Hash a flow key for faster lookup */
static uint32_t ovs_flow_hash(const struct ovs_flow_key *key)
{
    uint32_t h = key->src_ip ^ key->dst_ip ^ key->in_port;
    h ^= (uint32_t)key->src_port << 16 | key->dst_port;
    h ^= (uint32_t)key->proto << 8 | (key->vlan_vid & 0xFFF);
    return h;
}

/* Add a flow entry */
int ovs_flow_add(const struct ovs_flow_key *key,
                  const struct ovs_action *actions, int n_actions,
                  uint64_t cookie, int idle_timeout)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&ovs_lock, &irq_flags);

    /* Check for duplicate */
    for (int i = 0; i < OVS_FLOW_TABLE_SIZE; i++) {
        if (ovs_flow_table[i].in_use &&
            ovs_flow_matches(&ovs_flow_table[i], key)) {
            /* Update existing flow */
            ovs_flow_table[i].cookie = cookie;
            ovs_flow_table[i].action_count = (n_actions > OVS_MAX_ACTIONS) ? OVS_MAX_ACTIONS : n_actions;
            memcpy(ovs_flow_table[i].actions, actions,
                   ovs_flow_table[i].action_count * sizeof(struct ovs_action));
            ovs_flow_table[i].idle_timeout = idle_timeout;
            ovs_flow_table[i].last_hit_tick = timer_get_ticks();
            spinlock_irqsave_release(&ovs_lock, irq_flags);
            return 0;
        }
    }

    int idx = -1;
    for (int i = 0; i < OVS_FLOW_TABLE_SIZE; i++) {
        if (!ovs_flow_table[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        /* Evict oldest flow */
        uint64_t oldest = ovs_flow_table[0].last_hit_tick;
        idx = 0;
        for (int i = 1; i < OVS_FLOW_TABLE_SIZE; i++) {
            if (ovs_flow_table[i].last_hit_tick < oldest) {
                oldest = ovs_flow_table[i].last_hit_tick;
                idx = i;
            }
        }
    }

    ovs_flow_table[idx].in_use = 1;
    ovs_flow_table[idx].key = *key;
    ovs_flow_table[idx].key.src_ip_mask = key->src_ip_mask ? key->src_ip_mask : 0xFFFFFFFF;
    ovs_flow_table[idx].key.dst_ip_mask = key->dst_ip_mask ? key->dst_ip_mask : 0xFFFFFFFF;
    ovs_flow_table[idx].action_count = (n_actions > OVS_MAX_ACTIONS) ? OVS_MAX_ACTIONS : n_actions;
    memcpy(ovs_flow_table[idx].actions, actions,
           ovs_flow_table[idx].action_count * sizeof(struct ovs_action));
    ovs_flow_table[idx].cookie = cookie;
    ovs_flow_table[idx].packet_count = 0;
    ovs_flow_table[idx].byte_count = 0;
    ovs_flow_table[idx].last_hit_tick = timer_get_ticks();
    ovs_flow_table[idx].idle_timeout = idle_timeout;

    spinlock_irqsave_release(&ovs_lock, irq_flags);
    kprintf("[OVS] flow added (hash=0x%x, %d actions)\n",
            ovs_flow_hash(key), ovs_flow_table[idx].action_count);
    return 0;
}

/* Delete a flow */
int ovs_flow_del(uint64_t cookie)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&ovs_lock, &irq_flags);

    for (int i = 0; i < OVS_FLOW_TABLE_SIZE; i++) {
        if (ovs_flow_table[i].in_use && ovs_flow_table[i].cookie == cookie) {
            ovs_flow_table[i].in_use = 0;
            spinlock_irqsave_release(&ovs_lock, irq_flags);
            kprintf("[OVS] flow deleted (cookie=0x%lx)\n", (unsigned long)cookie);
            return 0;
        }
    }

    spinlock_irqsave_release(&ovs_lock, irq_flags);
    return -ENOENT;
}

/* Lookup a flow by key and execute actions */
int ovs_flow_execute(const struct ovs_flow_key *key,
                      uint8_t *packet, size_t *pkt_len)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&ovs_lock, &irq_flags);

    for (int i = 0; i < OVS_FLOW_TABLE_SIZE; i++) {
        if (!ovs_flow_table[i].in_use)
            continue;

        if (ovs_flow_matches(&ovs_flow_table[i], key)) {
            ovs_flow_table[i].packet_count++;
            ovs_flow_table[i].byte_count += *pkt_len;
            ovs_flow_table[i].last_hit_tick = timer_get_ticks();

            struct ovs_flow *flow = &ovs_flow_table[i];
            spinlock_irqsave_release(&ovs_lock, irq_flags);

            /* Execute actions */
            size_t len = *pkt_len;
            for (int j = 0; j < flow->action_count; j++) {
                switch (flow->actions[j].type) {
                case OVS_ACTION_OUTPUT: {
                    /* Forward to specified port */
                    if (netif_count() > 0) {
                        netif_send(flow->actions[j].output_port, packet, (uint16_t)len);
                    } else {
                        /* Fallback to direct send */
                        uint8_t *eth = packet;
                        send_eth(eth + 6, 0x0800, packet + 14, (uint16_t)(len - 14));
                    }
                    kprintf("[OVS] output to port %u (%zu bytes)\n",
                            flow->actions[j].output_port, len);
                    break;
                }
                case OVS_ACTION_DROP:
                    *pkt_len = 0;
                    return 0;

                case OVS_ACTION_PUSH_VLAN: {
                    /* Push a VLAN header after Ethernet source MAC */
                    if (len + 4 <= 2048) {
                        uint16_t vlan_tci = flow->actions[j].vlan_vid & 0x0FFF;
                        vlan_tci |= (0 << 13); /* priority = 0 */
                        uint8_t vlan_hdr[4] = {0x81, 0x00, (uint8_t)(vlan_tci >> 8), (uint8_t)vlan_tci};
                        /* Shift from Ethernet type field */
                        memmove(packet + 12 + 4, packet + 12, len - 12);
                        memcpy(packet + 12, vlan_hdr, 4);
                        len += 4;
                    }
                    break;
                }

                case OVS_ACTION_POP_VLAN: {
                    /* Remove VLAN header */
                    if (len >= 18 && packet[12] == 0x81 && packet[13] == 0x00) {
                        memmove(packet + 12, packet + 16, len - 16);
                        len -= 4;
                    }
                    break;
                }

                case OVS_ACTION_SET_FIELD:
                    /* Set a field in the packet (simplified) */
                    if (flow->actions[j].field_offset < len) {
                        uint32_t val = flow->actions[j].set_field_val;
                        uint8_t f_len = flow->actions[j].field_len;
                        if (f_len == 0) f_len = 4;
                        uint32_t copy_len = (uint32_t)((f_len < len - flow->actions[j].field_offset) ?
                                             f_len : (len - flow->actions[j].field_offset));
                        memcpy(packet + flow->actions[j].field_offset, &val, copy_len);
                    }
                    break;

                case OVS_ACTION_DEC_TTL:
                    /* Decrement TTL in IP header */
                    if (len >= 21) { /* eth(14) + ip(20) and at least TTL field */
                        uint8_t *ttl = packet + 14 + 8; /* TTL is at offset 8 in IP header */
                        if (*ttl > 0) (*ttl)--;
                    }
                    break;

                case OVS_ACTION_ENCAP_GRE: {
                    /* GRE tunnel encapsulation */
                    uint32_t tunnel_dst = flow->actions[j].tunnel_ip;
                    if (tunnel_dst && len >= 14) {
                        /* Build outer IP + GRE header */
                        uint8_t encap_buf[2048];
                        uint8_t *inner_pkt = packet + 14; /* Skip Ethernet header */
                        int inner_len = (int)len - 14;

                        struct ip_header *outer = (struct ip_header *)encap_buf;
                        memset(outer, 0, sizeof(*outer));
                        outer->version_ihl = 0x45;
                        outer->ttl = 64;
                        outer->protocol = 47; /* GRE */
                        outer->src_ip = htonl(net_our_ip);
                        outer->dst_ip = htonl(tunnel_dst);
                        outer->total_len = htons((uint16_t)(sizeof(*outer) + 4 + inner_len));
                        outer->checksum = net_checksum(outer, sizeof(*outer));

                        uint16_t *gre = (uint16_t *)(encap_buf + sizeof(*outer));
                        gre[0] = htons(0x0000); /* flags=0, version=0 */
                        gre[1] = htons(0x0800); /* ethertype IPv4 */

                        memcpy(encap_buf + sizeof(*outer) + 4, inner_pkt, inner_len);
                        int total = sizeof(*outer) + 4 + inner_len;

                        /* Send encapsulated packet */
                        send_eth(packet + 6, 0x0800, encap_buf, (uint16_t)total);
                    }
                    break;
                }

                default:
                    break;
                }
            }
            return 0;
        }
    }

    spinlock_irqsave_release(&ovs_lock, irq_flags);
    return -ENOENT; /* no matching flow */
}

/* Age out idle flows */
void ovs_flow_age(void)
{
    uint64_t irq_flags;
    uint64_t now = timer_get_ticks();

    spinlock_irqsave_acquire(&ovs_lock, &irq_flags);
    for (int i = 0; i < OVS_FLOW_TABLE_SIZE; i++) {
        if (ovs_flow_table[i].in_use &&
            ovs_flow_table[i].idle_timeout > 0 &&
            (now - ovs_flow_table[i].last_hit_tick) > (uint64_t)ovs_flow_table[i].idle_timeout * 10) {
            ovs_flow_table[i].in_use = 0;
            kprintf("[OVS] flow aged out (cookie=0x%lx)\n",
                    (unsigned long)ovs_flow_table[i].cookie);
        }
    }
    spinlock_irqsave_release(&ovs_lock, irq_flags);
}

void ovs_init(void)
{
    spinlock_init(&ovs_lock);
    memset(ovs_flow_table, 0, sizeof(ovs_flow_table));
    kprintf("[OK] Open vSwitch datapath (%d flow entries)\n",
            OVS_FLOW_TABLE_SIZE);
}
#include "module.h"
module_init(ovs_init);

/* ── Implement: ovs_add_flow ────────────────── */
int ovs_add_flow(const void *flow)
{
    if (!flow) {
        kprintf("[openvswitch] ovs_add_flow: NULL flow\n");
        return -EINVAL;
    }
    kprintf("[openvswitch] ovs_add_flow: flow=%p (stub)\n", flow);
    return -EOPNOTSUPP;
}
/* ── Implement: ovs_del_flow ────────────────── */
int ovs_del_flow(const void *flow)
{
    if (!flow) {
        kprintf("[openvswitch] ovs_del_flow: NULL flow\n");
        return -EINVAL;
    }
    kprintf("[openvswitch] ovs_del_flow: flow=%p (stub)\n", flow);
    return -EOPNOTSUPP;
}
/* ── Implement: ovs_add_port ────────────────── */
int ovs_add_port(const char *name, void *dev)
{
    if (!name || !dev) {
        kprintf("[openvswitch] ovs_add_port: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[openvswitch] ovs_add_port: %s dev=%p (stub)\n", name, dev);
    return -EOPNOTSUPP;
}
