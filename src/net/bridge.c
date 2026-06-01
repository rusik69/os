/* bridge.c — Learning bridge */

#define KERNEL_INTERNAL
#include "bridge.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

static struct bridge g_bridge;

int bridge_init(void) {
    if (g_bridge.initialized) return -1;
    memset(&g_bridge, 0, sizeof(g_bridge));
    g_bridge.initialized = 1;
    kprintf("[OK] Bridge initialized\\n");
    return 0;
}

int bridge_add_port(int port_iface) {
    if (!g_bridge.initialized) return -1;
    if (g_bridge.num_ports >= BRIDGE_MAX_PORTS) return -1;
    g_bridge.ports[g_bridge.num_ports++] = port_iface;
    return 0;
}

int bridge_remove_port(int port_iface) {
    if (!g_bridge.initialized) return -1;
    for (int i = 0; i < g_bridge.num_ports; i++) {
        if (g_bridge.ports[i] == port_iface) {
            for (int j = i; j < g_bridge.num_ports - 1; j++)
                g_bridge.ports[j] = g_bridge.ports[j + 1];
            g_bridge.num_ports--;
            return 0;
        }
    }
    return -1;
}

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

void bridge_handle(const uint8_t *frame, int len, int ingress_port) {
    if (!g_bridge.initialized) return;
    if (!frame || len < 14) return;

    const uint8_t *dst_mac = frame;
    const uint8_t *src_mac = frame + 6;

    /* Learn source MAC */
    bridge_fdb_learn(src_mac, ingress_port);

    /* Look up destination */
    int dst_port = bridge_fdb_lookup(dst_mac);

    /* Check if broadcast or unknown unicast: flood to all ports */
    int is_broadcast = 1;
    for (int i = 0; i < 6; i++) {
        if (dst_mac[i] != 0xFF) { is_broadcast = 0; break; }
    }

    if (dst_port < 0 || is_broadcast) {
        /* Flood to all ports except ingress */
        for (int i = 0; i < g_bridge.num_ports; i++) {
            if (g_bridge.ports[i] != ingress_port) {
                /* Forward frame to this port */
                /* TODO: call netdev transmit on port */
                (void)g_bridge.ports[i];
            }
        }
    } else if (dst_port != ingress_port) {
        /* Forward to specific port */
        /* TODO: call netdev transmit on dst_port */
        (void)dst_port;
    }
    /* Else: destination is on same port — drop */
}
