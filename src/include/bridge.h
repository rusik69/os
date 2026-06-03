#ifndef BRIDGE_H
#define BRIDGE_H

#include "types.h"

/* FDB table size */
#define BRIDGE_FDB_SIZE 64
#define BRIDGE_MAX_PORTS 8
#define BRIDGE_FDB_AGE_TICKS 3000  /* 300 seconds at 10 ticks/s */

/* ── IGMP snooping constants ────────────────────────────────────── */
#define BRIDGE_IGMP_MAX_GROUPS  32   /* max multicast groups tracked */
#define BRIDGE_IGMP_AGE_TICKS  3000 /* age out after 300 s (match FDB) */

/* IGMP snooping entry: tracks which ports want a multicast group */
struct bridge_mcast_entry {
    uint8_t  group_mac[6];     /* multicast destination MAC (derived from IP) */
    uint32_t group_ip;         /* multicast group IP (network order) */
    uint32_t port_mask;        /* bitmask of ports that have joined */
    uint64_t last_report_tick; /* time of last membership report */
    int      valid;
};

/* FDB entry */
struct bridge_fdb_entry {
    uint8_t  mac[6];
    int      port;
    uint64_t learn_tick;
    int      valid;
};

/* Bridge state */
struct bridge {
    int          num_ports;
    int          ports[BRIDGE_MAX_PORTS];
    struct bridge_fdb_entry fdb[BRIDGE_FDB_SIZE];
    struct bridge_mcast_entry mcast[BRIDGE_IGMP_MAX_GROUPS];
    uint8_t      bridge_mac[6];
    int          initialized;
};

/* ── API ────────────────────────────────────────────────────────── */

int  bridge_init(void);
int  bridge_add_port(int port_iface);
int  bridge_remove_port(int port_iface);
void bridge_handle(const uint8_t *frame, int len, int ingress_port);
int  bridge_fdb_lookup(const uint8_t *mac);
void bridge_fdb_learn(const uint8_t *mac, int port);
void bridge_fdb_age(void);
void bridge_fdb_flush(void);

/* ── IGMP snooping API ──────────────────────────────────────────── */

/* Process an IGMP report/leave seen on a bridge port.
 * Called from bridge_handle when an IGMP packet is detected. */
void bridge_igmp_snoop(const uint8_t *frame, int len, int ingress_port);

/* Query whether a given multicast MAC has any subscribers.
 * Returns the port_mask of interested ports, or 0 if none. */
uint32_t bridge_igmp_lookup(const uint8_t *mcast_mac);

/* Age out stale IGMP snooping entries. */
void bridge_igmp_age(void);

#endif /* BRIDGE_H */
