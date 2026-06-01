#ifndef BRIDGE_H
#define BRIDGE_H

#include "types.h"

/* FDB table size */
#define BRIDGE_FDB_SIZE 64
#define BRIDGE_MAX_PORTS 8
#define BRIDGE_FDB_AGE_TICKS 3000  /* 300 seconds at 10 ticks/s */

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

#endif /* BRIDGE_H */
