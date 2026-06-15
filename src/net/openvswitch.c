// SPDX-License-Identifier: GPL-2.0-only
/*
 * openvswitch.c — Open vSwitch datapath skeleton
 *
 * Provides the Open vSwitch datapath for software-defined networking.
 * Implements flow table matching, action execution, and tunneling.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"

#define OVS_FLOW_TABLE_SIZE 1024
#define OVS_MAX_ACTIONS     16

/* Flow key (simplified) */
struct ovs_flow_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint32_t in_port;
};

/* Action types */
#define OVS_ACTION_OUTPUT     1
#define OVS_ACTION_DROP       2
#define OVS_ACTION_SET_FIELD  3
#define OVS_ACTION_PUSH_VLAN  4
#define OVS_ACTION_POP_VLAN   5

struct ovs_action {
    int type;
    union {
        uint32_t output_port;
        uint32_t set_field_val;
        uint16_t vlan_vid;
    };
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
    int idle_timeout;
};

static struct ovs_flow ovs_flow_table[OVS_FLOW_TABLE_SIZE];
static spinlock_t ovs_lock;

/* Add a flow entry */
int ovs_flow_add(const struct ovs_flow_key *key,
                  const struct ovs_action *actions, int n_actions,
                  uint64_t cookie, int idle_timeout)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&ovs_lock, &irq_flags);

    int idx = -1;
    for (int i = 0; i < OVS_FLOW_TABLE_SIZE; i++) {
        if (!ovs_flow_table[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        spinlock_irqsave_release(&ovs_lock, irq_flags);
        return -ENOMEM;
    }

    ovs_flow_table[idx].in_use = 1;
    ovs_flow_table[idx].key = *key;
    ovs_flow_table[idx].action_count = (n_actions > OVS_MAX_ACTIONS) ? OVS_MAX_ACTIONS : n_actions;
    memcpy(ovs_flow_table[idx].actions, actions,
           ovs_flow_table[idx].action_count * sizeof(struct ovs_action));
    ovs_flow_table[idx].cookie = cookie;
    ovs_flow_table[idx].packet_count = 0;
    ovs_flow_table[idx].byte_count = 0;
    ovs_flow_table[idx].idle_timeout = idle_timeout;

    spinlock_irqsave_release(&ovs_lock, irq_flags);
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

        /* Simple exact-match lookup */
        if (memcmp(&ovs_flow_table[i].key, key, sizeof(struct ovs_flow_key)) == 0) {
            ovs_flow_table[i].packet_count++;
            ovs_flow_table[i].byte_count += *pkt_len;

            struct ovs_flow *flow = &ovs_flow_table[i];
            spinlock_irqsave_release(&ovs_lock, irq_flags);

            /* Execute actions */
            for (int j = 0; j < flow->action_count; j++) {
                switch (flow->actions[j].type) {
                case OVS_ACTION_OUTPUT:
                    kprintf("[OVS] output to port %u\n", flow->actions[j].output_port);
                    break;
                case OVS_ACTION_DROP:
                    *pkt_len = 0;
                    return 0;
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

void ovs_init(void)
{
    spinlock_init(&ovs_lock);
    memset(ovs_flow_table, 0, sizeof(ovs_flow_table));
    kprintf("[OK] Open vSwitch datapath skeleton (%d flow entries)\n",
            OVS_FLOW_TABLE_SIZE);
}
#include "module.h"
module_init(ovs_init);
