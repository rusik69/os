#ifndef BONDING_H
#define BONDING_H

#include "types.h"
#include "netdevice.h"

/*
 * ── Bonding Driver (IEEE 802.3ad-inspired) ──────────────────────
 *
 * Virtual driver that bonds multiple net_device interfaces into a
 * single logical interface (bond0).  Supports four bonding modes:
 *
 *   0 = balance-rr     Round-robin across slaves
 *   1 = active-backup  One active slave; failover on link down
 *   2 = balance-xor    XOR of MAC addresses selects slave
 *   3 = broadcast      All slaves transmit each frame
 *
 * Slaves are managed via /sys/class/net/bond0/bonding/slaves
 * (write "+eth0" to add, "-eth0" to remove).
 */

/* ── Bonding limits ───────────────────────────────────────────── */
#define BONDING_MAX_SLAVES  4
#define BONDING_NAME_MAX    NETDEV_NAME_MAX

/* ── Bonding modes ────────────────────────────────────────────── */
#define BOND_MODE_ROUNDROBIN   0
#define BOND_MODE_ACTIVEBACKUP 1
#define BOND_MODE_XOR          2
#define BOND_MODE_BROADCAST    3

/* ── Slave state flags ────────────────────────────────────────── */
#define BOND_SLAVE_ACTIVE   (1 << 0)
#define BOND_SLAVE_UP       (1 << 1)

/* ── Per-slave descriptor ─────────────────────────────────────── */
struct bond_slave {
    int               ifindex;       /* net_device ifindex */
    uint8_t           state;         /* BOND_SLAVE_* flags */
    uint8_t           mac[6];        /* cached MAC */
};

/* ── Bonding master device (private data) ──────────────────────── */
struct bonding {
    char              name[BONDING_NAME_MAX];
    int               mode;          /* BOND_MODE_* */
    int               bonded;        /* 1 = registered with netdevice layer */
    int               ifindex;       /* our net_device ifindex (bond0) */
    int               slave_count;
    struct bond_slave slaves[BONDING_MAX_SLAVES];
    int               rr_counter;    /* round-robin counter */
    int               active_slave;  /* index into slaves[] for active-backup */
    /* Failover detection */
    int               link_monitor_ticks;  /* ticks between link checks */
    uint64_t          last_link_check;
};

/* ── API ──────────────────────────────────────────────────────── */

/* Initialise the bonding subsystem (called at boot) */
void bonding_init(void);

/* Create a new bonding master device.
 * @name: interface name (e.g. "bond0")
 * @mode: BOND_MODE_* constant
 * Returns 0 on success, -1 on failure. */
int bonding_create(const char *name, int mode);

/* Destroy a bonding master device.
 * Releases all slaves and unregisters the interface. */
int bonding_destroy(const char *name);

/* Add a slave by interface name to a bonding master.
 * @bond_name: bonding master name (e.g. "bond0")
 * @slave_name: slave interface name (e.g. "eth0")
 * Returns 0 on success, -1 on failure. */
int bonding_add_slave(const char *bond_name, const char *slave_name);

/* Remove a slave by interface name from a bonding master. */
int bonding_remove_slave(const char *bond_name, const char *slave_name);

/* Find a bonding master by its net_device ifindex. */
struct bonding *bonding_find_by_ifindex(int ifindex);

/* Periodic link monitor (call from net_poll or timer).
 * Checks slave link state and performs failover if needed. */
void bonding_link_monitor(void);

#endif /* BONDING_H */
