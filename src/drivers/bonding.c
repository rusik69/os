#define KERNEL_INTERNAL
#include "bonding.h"
#include "netdevice.h"
#include "net.h"           /* for net_rx_dispatch */
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "timers.h"
#include "errno.h"
#include "sysfs.h"
#include "spinlock.h"

/*
 * ── Bonding Driver Implementation ──────────────────────────────────
 *
 * Implements a virtual net_device that distributes traffic across
 * multiple real slave interfaces according to the bonding mode.
 *
 * Transmit path:
 *   1. bonding_transmit() is the net_device transmit callback.
 *   2. It selects one or more slaves based on the mode.
 *   3. Calls netif_send() on the selected slave(s).
 *
 * Receive path:
 *   Slaves receive packets independently.  The bonding layer does
 *   NOT filter inbound traffic — all slaves are up and process
 *   packets normally.  This is a simple L2 bond.
 *
 * Link monitoring:
 *   bonding_link_monitor() polls slave link state and triggers
 *   failover in active-backup mode.
 */

/* ── Global state ───────────────────────────────────────────────── */

static struct bonding g_bonding_masters[NETDEV_MAX];
static int g_bonding_count = 0;
static int g_bonding_initialized = 0;
static spinlock_t g_bond_lock = SPINLOCK_INIT;

/* ── Forward declarations of net_device callbacks ──────────────────── */

static int bonding_transmit(struct net_device *dev,
                             const uint8_t *data, uint16_t len);
static int bonding_receive(struct net_device *dev,
                            uint8_t *buf, uint16_t max_len);

/* ── Helpers ──────────────────────────────────────────────────────── */

static struct bonding *bonding_find_by_name(const char *name)
{
    for (int i = 0; i < g_bonding_count; i++) {
        if (strcmp(g_bonding_masters[i].name, name) == 0)
            return &g_bonding_masters[i];
    }
    return NULL;
}

struct bonding *bonding_find_by_ifindex(int ifindex)
{
    for (int i = 0; i < g_bonding_count; i++) {
        if (g_bonding_masters[i].bonded &&
            g_bonding_masters[i].ifindex == ifindex)
            return &g_bonding_masters[i];
    }
    return NULL;
}

static int bonding_has_slave(struct bonding *bond, int ifindex)
{
    for (int i = 0; i < bond->slave_count; i++) {
        if (bond->slaves[i].ifindex == ifindex)
            return 1;
    }
    return 0;
}

/* ── Transmit path ────────────────────────────────────────────────── */

static int tx_roundrobin(struct bonding *bond,
                          const uint8_t *data, uint16_t len)
{
    if (bond->slave_count == 0) return -1;

    int start = bond->rr_counter % bond->slave_count;
    bond->rr_counter++;

    int ifindex = bond->slaves[start].ifindex;
    return netif_send(ifindex, data, len);
}

static int tx_active_backup(struct bonding *bond,
                             const uint8_t *data, uint16_t len)
{
    if (bond->slave_count == 0) return -1;

    int idx = bond->active_slave;

    /* If the active slave index is out of bounds, or the active slave's
     * link is down, scan for an alternative UP slave to prevent packet
     * loss during the failover detection window (~1 sec for timer). */
    if (idx < 0 || idx >= bond->slave_count ||
        !(bond->slaves[idx].state & BOND_SLAVE_UP)) {

        int found = -1;
        for (int i = 0; i < bond->slave_count; i++) {
            if (bond->slaves[i].state & BOND_SLAVE_UP) {
                found = i;
                break;
            }
        }

        if (found >= 0) {
            idx = found;
            bond->active_slave = found;
        } else {
            /* No slaves are UP; fall back to first slave anyway */
            idx = 0;
            bond->active_slave = 0;
        }
    }

    return netif_send(bond->slaves[idx].ifindex, data, len);
}

static int tx_xor(struct bonding *bond,
                   const uint8_t *data, uint16_t len)
{
    if (bond->slave_count == 0) return -1;

    /* Simple XOR hash over source and destination MACs */
    uint8_t hash = 0;
    if (len >= 12) {
        for (int i = 0; i < 12; i++)
            hash ^= data[i];
    }
    int idx = hash % bond->slave_count;
    return netif_send(bond->slaves[idx].ifindex, data, len);
}

static int tx_broadcast(struct bonding *bond,
                         const uint8_t *data, uint16_t len)
{
    if (bond->slave_count == 0) return -1;

    int last_err = 0;
    for (int i = 0; i < bond->slave_count; i++) {
        int ret = netif_send(bond->slaves[i].ifindex, data, len);
        if (ret < 0) last_err = ret;
    }
    return last_err;
}

static int bonding_transmit(struct net_device *dev,
                             const uint8_t *data, uint16_t len)
{
    struct bonding *bond = (struct bonding *)dev->priv;
    if (!bond) return -1;

    switch (bond->mode) {
        case BOND_MODE_ROUNDROBIN:
            return tx_roundrobin(bond, data, len);
        case BOND_MODE_ACTIVEBACKUP:
            return tx_active_backup(bond, data, len);
        case BOND_MODE_XOR:
            return tx_xor(bond, data, len);
        case BOND_MODE_BROADCAST:
            return tx_broadcast(bond, data, len);
        default:
            return -1;
    }
}

/* ── Receive path (bonding itself has no RX buffer) ───────────────── */

static int bonding_receive(struct net_device *dev,
                            uint8_t *buf, uint16_t max_len)
{
    (void)dev;
    (void)buf;
    (void)max_len;
    /* Bonding does not provide its own Rx buffer; slaves deliver
     * packets directly through their own receive paths.  The bond
     * master's receive callback is never invoked by the current
     * polling architecture — it exists so the net_device is valid. */
    return 0;
}

/* ── Link monitoring / failover (active-backup) ───────────────────── */

void bonding_link_monitor(void)
{
    if (!g_bonding_initialized) return;
    if (!timer_available()) return;

    uint64_t now = timer_get_ticks();
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bond_lock, &irq_flags);

    for (int b = 0; b < g_bonding_count; b++) {
        struct bonding *bond = &g_bonding_masters[b];
        if (!bond->bonded || bond->mode != BOND_MODE_ACTIVEBACKUP)
            continue;

        /* Check every ~100 ticks (roughly 1 second @ 100Hz) */
        if (now - bond->last_link_check < 100)
            continue;
        bond->last_link_check = now;

        /* Poll each slave's link state via netif_recv (returns -1 or 0
         * if the device is not reachable).  In this simple implementation
         * we just check whether the slave interface is still registered. */
        int active_up = 0;
        for (int i = 0; i < bond->slave_count; i++) {
            struct net_device *slave = netif_get(bond->slaves[i].ifindex);
            if (slave) {
                bond->slaves[i].state = (uint8_t)(bond->slaves[i].state | BOND_SLAVE_UP);
                if (i == bond->active_slave)
                    active_up = 1;
            } else {
                bond->slaves[i].state = (uint8_t)(bond->slaves[i].state & ~BOND_SLAVE_UP);
            }
        }

        /* Failover: if active slave is down, switch to first up slave */
        if (!active_up && bond->slave_count > 0) {
            /* Clear ACTIVE flag from the current active slave */
            if (bond->active_slave >= 0 && bond->active_slave < bond->slave_count)
                bond->slaves[bond->active_slave].state &= ~BOND_SLAVE_ACTIVE;

            for (int i = 0; i < bond->slave_count; i++) {
                if (bond->slaves[i].state & BOND_SLAVE_UP) {
                    bond->active_slave = i;
                    bond->slaves[i].state |= BOND_SLAVE_ACTIVE;
                    kprintf("[BOND] %s: failover to slave %d (ifindex %d)\n",
                            bond->name, i, bond->slaves[i].ifindex);
                    break;
                }
            }
        }
    }

    spinlock_irqsave_release(&g_bond_lock, irq_flags);
}

/* ── Slave management ─────────────────────────────────────────────── */

int bonding_add_slave(const char *bond_name, const char *slave_name)
{
    if (!bond_name || !slave_name) return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bond_lock, &irq_flags);

    struct bonding *bond = bonding_find_by_name(bond_name);
    if (!bond || !bond->bonded) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -ENODEV;
    }

    if (bond->slave_count >= BONDING_MAX_SLAVES) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -ENOSPC;
    }

    /* Resolve slave interface name to ifindex */
    int ifindex = netif_name_to_index(slave_name);
    if (ifindex < 0) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -ENODEV;
    }

    /* Check for duplicate */
    if (bonding_has_slave(bond, ifindex)) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -EEXIST;
    }

    /* Add slave */
    int idx = bond->slave_count;
    bond->slaves[idx].ifindex = ifindex;
    bond->slaves[idx].state = BOND_SLAVE_UP;
    if (bond->slave_count == 0)
        bond->slaves[idx].state |= BOND_SLAVE_ACTIVE;
    struct net_device *nd = netif_get(ifindex);
    if (nd) {
        memcpy(bond->slaves[idx].mac, nd->mac, 6);
    }
    bond->slave_count++;

    /* If this is the first slave, set it as active */
    if (bond->slave_count == 1)
        bond->active_slave = 0;

    kprintf("[BOND] %s: added slave %s (ifindex %d)\n",
            bond_name, slave_name, ifindex);

    spinlock_irqsave_release(&g_bond_lock, irq_flags);
    return 0;
}

int bonding_remove_slave(const char *bond_name, const char *slave_name)
{
    if (!bond_name || !slave_name) return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bond_lock, &irq_flags);

    struct bonding *bond = bonding_find_by_name(bond_name);
    if (!bond || !bond->bonded) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -ENODEV;
    }

    int ifindex = netif_name_to_index(slave_name);
    if (ifindex < 0) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -ENODEV;
    }

    int found = -1;
    for (int i = 0; i < bond->slave_count; i++) {
        if (bond->slaves[i].ifindex == ifindex) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -ENOENT;
    }

    /* Shift remaining slaves down */
    for (int i = found; i < bond->slave_count - 1; i++)
        bond->slaves[i] = bond->slaves[i + 1];
    bond->slave_count--;

    /* Adjust active_slave when removing a slave */
    if (found < bond->active_slave) {
        /* A slave before the active one was removed; shift index down */
        bond->active_slave--;
    } else if (found == bond->active_slave) {
        /* The active slave itself was removed; reset to first available */
        bond->active_slave = 0;
    }
    /* If found > active_slave, no adjustment needed (position unchanged) */

    /* Safety: ensure active_slave stays in bounds */
    if (bond->active_slave >= bond->slave_count && bond->slave_count > 0)
        bond->active_slave = 0;

    kprintf("[BOND] %s: removed slave %s (ifindex %d)\n",
            bond_name, slave_name, ifindex);

    spinlock_irqsave_release(&g_bond_lock, irq_flags);
    return 0;
}

/* ── Sysfs helpers ─────────────────────────────────────────────────── */

/* Read /sys/class/net/bond0/bonding/slaves */
static int slaves_read_cb(char *buf, uint32_t max_size, void *priv)
{
    struct bonding *bond = (struct bonding *)priv;
    if (!bond) return 0;

    int pos = 0;
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bond_lock, &irq_flags);

    for (int i = 0; i < bond->slave_count && pos < (int)max_size - 16; i++) {
        struct net_device *slave_nd = netif_get(bond->slaves[i].ifindex);
        const char *name = slave_nd ? slave_nd->name : "?";
        int n = snprintf(buf + pos, (size_t)(max_size - (uint32_t)pos),
                         "%s\n", name);
        if (n > 0 && pos + n < (int)max_size) pos += n;
    }

    spinlock_irqsave_release(&g_bond_lock, irq_flags);
    return pos;
}

/* Write /sys/class/net/bond0/bonding/slaves — "+eth0" to add, "-eth0" to remove */
static int slaves_write_cb(const char *data, uint32_t size, void *priv)
{
    struct bonding *bond = (struct bonding *)priv;
    if (!bond || !data || size < 2) return -1;

    char op = data[0];
    char name[NETDEV_NAME_MAX];
    int name_len = (int)size - 1;
    if (name_len >= NETDEV_NAME_MAX) name_len = NETDEV_NAME_MAX - 1;
    memcpy(name, data + 1, (size_t)name_len);
    name[name_len] = '\0';

    /* Strip trailing newline if present */
    while (name_len > 0 && (name[name_len - 1] == '\n' || name[name_len - 1] == '\r'))
        name[--name_len] = '\0';

    if (op == '+') {
        int ret = bonding_add_slave(bond->name, name);
        if (ret < 0) return ret;
        return 0;
    } else if (op == '-') {
        int ret = bonding_remove_slave(bond->name, name);
        if (ret < 0) return ret;
        return 0;
    }

    return -1;
}

/* Read /sys/class/net/bond0/bonding/mode */
static int mode_read_cb(char *buf, uint32_t max_size, void *priv)
{
    struct bonding *bond = (struct bonding *)priv;
    if (!bond) return 0;

    static const char *const mode_names[] = {
        "balance-rr (0)",
        "active-backup (1)",
        "balance-xor (2)",
        "broadcast (3)"
    };

    const char *mstr = (bond->mode >= 0 && bond->mode <= 3)
                       ? mode_names[bond->mode] : "unknown";
    return snprintf(buf, (size_t)max_size, "%s\n", mstr);
}

static void bonding_create_sysfs(struct bonding *bond)
{
    char path[64];

    /* Create /sys/class/net/bond0/bonding/ directory */
    snprintf(path, sizeof(path), "/sys/class/net/%s", bond->name);
    sysfs_create_dir(path);

    snprintf(path, sizeof(path), "/sys/class/net/%s/bonding", bond->name);
    sysfs_create_dir(path);

    /* Create slaves file (writable) */
    snprintf(path, sizeof(path), "/sys/class/net/%s/bonding/slaves", bond->name);
    sysfs_create_writable_file(path, "", bond, slaves_read_cb, slaves_write_cb);

    /* Create mode file (read-only) */
    snprintf(path, sizeof(path), "/sys/class/net/%s/bonding/mode", bond->name);
    sysfs_create_writable_file(path, "", bond, mode_read_cb, NULL);
}

/* ── Create / destroy bonding master ────────────────────────────────── */

int bonding_create(const char *name, int mode)
{
    if (!name || mode < 0 || mode > 3) return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bond_lock, &irq_flags);

    if (g_bonding_count >= NETDEV_MAX) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -ENOSPC;
    }

    /* Check for duplicate name */
    if (bonding_find_by_name(name)) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -EEXIST;
    }

    struct bonding *bond = &g_bonding_masters[g_bonding_count];
    memset(bond, 0, sizeof(*bond));
    strncpy(bond->name, name, BONDING_NAME_MAX - 1);
    bond->name[BONDING_NAME_MAX - 1] = '\0';
    bond->mode = mode;
    bond->rr_counter = 0;
    bond->active_slave = 0;
    bond->last_link_check = 0;

    /* Register as a net_device */
    struct net_device nd;
    memset(&nd, 0, sizeof(nd));
    strncpy(nd.name, name, NETDEV_NAME_MAX - 1);
    nd.name[NETDEV_NAME_MAX - 1] = '\0';
    nd.transmit = bonding_transmit;
    nd.receive = bonding_receive;
    nd.mtu = 1500;
    nd.flags = 1; /* IFF_UP */
    nd.priv = (void *)bond;

    int ifindex = netif_register(&nd);
    if (ifindex < 0) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -ENOMEM;
    }

    bond->ifindex = ifindex;
    bond->bonded = 1;
    g_bonding_count++;

    bonding_create_sysfs(bond);

    kprintf("[OK] Bonding master %s created (mode %d, ifindex %d)\n",
            name, mode, ifindex);

    spinlock_irqsave_release(&g_bond_lock, irq_flags);
    return 0;
}

int bonding_destroy(const char *name)
{
    if (!name) return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bond_lock, &irq_flags);

    int found = -1;
    for (int i = 0; i < g_bonding_count; i++) {
        if (strcmp(g_bonding_masters[i].name, name) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        spinlock_irqsave_release(&g_bond_lock, irq_flags);
        return -ENODEV;
    }

    struct bonding *bond = &g_bonding_masters[found];

    /* Unregister sysfs entries */
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/bonding/slaves", name);
    sysfs_remove(path);
    snprintf(path, sizeof(path), "/sys/class/net/%s/bonding/mode", name);
    sysfs_remove(path);
    snprintf(path, sizeof(path), "/sys/class/net/%s/bonding", name);
    sysfs_remove(path);

    /* Unregister net_device */
    netif_unregister(bond->ifindex);
    bond->bonded = 0;
    bond->slave_count = 0;

    /* Shift remaining bonds down */
    for (int i = found; i < g_bonding_count - 1; i++)
        g_bonding_masters[i] = g_bonding_masters[i + 1];
    g_bonding_count--;

    kprintf("[BOND] Bonding master %s destroyed\n", name);

    spinlock_irqsave_release(&g_bond_lock, irq_flags);
    return 0;
}

/* ── Initialisation ─────────────────────────────────────────────────── */

void bonding_init(void)
{
    g_bonding_initialized = 1;
    kprintf("[OK] Bonding driver initialized\n");

    /* Create default bond0 interface */
    bonding_create("bond0", BOND_MODE_ACTIVEBACKUP);
}

/* ── Stub: bond_open ─────────────────────────────── */
static int bond_open(void *dev)
{
    (void)dev;
    kprintf("[BONDING] bond_open: not yet implemented\n");
    return 0;
}
/* ── Stub: bond_stop ─────────────────────────────── */
static int bond_stop(void *dev)
{
    (void)dev;
    kprintf("[BONDING] bond_stop: not yet implemented\n");
    return 0;
}
/* ── Stub: bond_xmit ─────────────────────────────── */
static int bond_xmit(void *skb, void *dev)
{
    (void)skb;
    (void)dev;
    kprintf("[BONDING] bond_xmit: not yet implemented\n");
    return 0;
}
/* ── Stub: bond_enslave ─────────────────────────────── */
static int bond_enslave(void *dev, void *slave_dev)
{
    (void)dev;
    (void)slave_dev;
    kprintf("[BONDING] bond_enslave: not yet implemented\n");
    return 0;
}
/* ── Stub: bond_release ─────────────────────────────── */
static int bond_release(void *dev, void *slave_dev)
{
    (void)dev;
    (void)slave_dev;
    kprintf("[BONDING] bond_release: not yet implemented\n");
    return 0;
}
