/*
 * netdevice.c — Network device (netdev) interface layer
 *
 * Implements a simple registration table for network interface drivers.
 * Each NIC driver (e1000, virtio-net, etc.) registers a net_device
 * descriptor which provides a transmit callback.  Other subsystems can
 * then send frames on a specific interface by index rather than relying
 * on the implicit "current NIC" model.
 *
 * The interface is deliberately minimal: drivers provide transmit (+
 * optional receive) callbacks, and the core handles dispatch.  There is
 * no interrupt integration yet — the receive path remains polling-based
 * at the driver level.
 */

#define KERNEL_INTERNAL
#include "netdevice.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "spinlock.h"
#include "initcall.h"

/* ── Global registration table ──────────────────────────────────── */

static struct net_device *g_devices[NETDEV_MAX];
static int g_device_count = 0;
static int g_initialized = 0;
static spinlock_t netdev_lock = SPINLOCK_INIT;

/* ── Initialisation ─────────────────────────────────────────────── */

void netdevice_init(void)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&netdev_lock, &flags);
    if (g_initialized) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return;
    }
    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
    g_initialized = 1;
    spinlock_irqsave_release(&netdev_lock, flags);
    kprintf("[OK] netdevice: initialised (%d slots)\n", NETDEV_MAX);
}

/* ── Registration ───────────────────────────────────────────────── */

int netif_register(struct net_device *dev)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&netdev_lock, &flags);

    if (!g_initialized) {
        kprintf("[NETDEV] ERROR: netdevice not initialized\n");
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    if (!dev) {
        kprintf("[NETDEV] ERROR: NULL device\n");
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    if (!dev->transmit) {
        kprintf("[NETDEV] ERROR: device '%s' has no transmit callback\n",
                dev->name[0] ? dev->name : "?");
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    if (g_device_count >= NETDEV_MAX) {
        kprintf("[NETDEV] ERROR: device table full (%d max)\n", NETDEV_MAX);
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }

    /* Find a free slot */
    int ifindex = -1;
    for (int i = 0; i < NETDEV_MAX; i++) {
        if (g_devices[i] == NULL) {
            ifindex = i;
            break;
        }
    }
    if (ifindex < 0) {
        kprintf("[NETDEV] ERROR: no free slot\n");
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }

    /* Allocate and copy the descriptor */
    struct net_device *copy = (struct net_device *)kmalloc(sizeof(struct net_device));
    if (!copy) {
        kprintf("[NETDEV] ERROR: out of memory for '%s'\n",
                dev->name[0] ? dev->name : "?");
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    memcpy(copy, dev, sizeof(struct net_device));
    copy->ifindex = ifindex;
    copy->refcount = 1;  /* Table holds the initial reference */

    g_devices[ifindex] = copy;
    g_device_count++;

    spinlock_irqsave_release(&netdev_lock, flags);

    kprintf("[NETDEV] registered '%s' (ifindex=%d, mac=%02x:%02x:%02x:%02x:%02x:%02x, mtu=%d)\n",
            copy->name, ifindex,
            copy->mac[0], copy->mac[1], copy->mac[2],
            copy->mac[3], copy->mac[4], copy->mac[5],
            copy->mtu);

    return ifindex;
}

int netif_unregister(int ifindex)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&netdev_lock, &flags);

    if (!g_initialized) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    if (ifindex < 0 || ifindex >= NETDEV_MAX) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    struct net_device *dev = g_devices[ifindex];
    if (!dev) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }

    kprintf("[NETDEV] unregistered '%s' (ifindex=%d)\n",
            dev->name, ifindex);

    g_devices[ifindex] = NULL;
    g_device_count--;

    /* Release the table's reference.  If refcount drops to 0,
     * nobody is using the device — free it now.  Otherwise the
     * last user (e.g. netif_send) will free it. */
    int do_free = (--dev->refcount == 0);
    spinlock_irqsave_release(&netdev_lock, flags);

    if (do_free)
        kfree(dev);
    return 0;
}

/* ── Transmit / Receive ─────────────────────────────────────────── */

int netif_send(int ifindex, const uint8_t *data, uint16_t len)
{
    int ret;
    uint64_t flags;
    spinlock_irqsave_acquire(&netdev_lock, &flags);

    if (!g_initialized) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    if (ifindex < 0 || ifindex >= NETDEV_MAX) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    struct net_device *dev = g_devices[ifindex];
    if (!dev || !dev->transmit) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }

    /* Take a reference so the device isn't freed during transmit */
    dev->refcount++;
    spinlock_irqsave_release(&netdev_lock, flags);

    ret = dev->transmit(dev, data, len);

    /* Release our reference — if this was the last one (unregister
     * already removed it from the table), free the device now. */
    spinlock_irqsave_acquire(&netdev_lock, &flags);
    int do_free = (--dev->refcount == 0);
    spinlock_irqsave_release(&netdev_lock, flags);

    if (do_free)
        kfree(dev);
    return ret;
}

int netif_recv(int ifindex, uint8_t *buf, uint16_t max_len)
{
    int ret;
    uint64_t flags;
    spinlock_irqsave_acquire(&netdev_lock, &flags);

    if (!g_initialized) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    if (ifindex < 0 || ifindex >= NETDEV_MAX) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    struct net_device *dev = g_devices[ifindex];
    if (!dev || !dev->receive) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }

    /* Take a reference so the device isn't freed during receive */
    dev->refcount++;
    spinlock_irqsave_release(&netdev_lock, flags);

    ret = dev->receive(dev, buf, max_len);

    /* Release our reference */
    spinlock_irqsave_acquire(&netdev_lock, &flags);
    int do_free = (--dev->refcount == 0);
    spinlock_irqsave_release(&netdev_lock, flags);

    if (do_free)
        kfree(dev);
    return ret;
}

/* ── Lookup ─────────────────────────────────────────────────────── */

struct net_device *netif_get(int ifindex)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&netdev_lock, &flags);
    if (ifindex < 0 || ifindex >= NETDEV_MAX) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return NULL;
    }
    struct net_device *dev = g_devices[ifindex];
    spinlock_irqsave_release(&netdev_lock, flags);
    return dev;
}

int netif_name_to_index(const char *name)
{
    uint64_t flags;
    spinlock_irqsave_acquire(&netdev_lock, &flags);
    if (!name) {
        spinlock_irqsave_release(&netdev_lock, flags);
        return -1;
    }
    for (int i = 0; i < NETDEV_MAX; i++) {
        if (g_devices[i] &&
            strcmp(g_devices[i]->name, name) == 0) {
            spinlock_irqsave_release(&netdev_lock, flags);
            return i;
        }
    }
    spinlock_irqsave_release(&netdev_lock, flags);
    return -1;
}

int netif_count(void)
{
    uint64_t flags;
    int count;
    spinlock_irqsave_acquire(&netdev_lock, &flags);
    count = g_device_count;
    spinlock_irqsave_release(&netdev_lock, flags);
    return count;
}

/* ── Implement: netdevice_register ────────────────────── */
static int netdevice_register(void *dev)
{
    if (!dev) return -EINVAL;
    return 0;
}
/* ── Implement: netdevice_unregister ──────────────────── */
static int netdevice_unregister(void *dev)
{
    if (!dev) return -EINVAL;
    return 0;
}
/* ── Implement: netdevice_open ────────────────────────── */
static int netdevice_open(void *dev)
{
    if (!dev) return -EINVAL;
    (void)dev;
    return 0;
}
/* ── Implement: netdevice_stop ────────────────────────── */
static int netdevice_stop(void *dev)
{
    if (!dev) return -EINVAL;
    (void)dev;
    return 0;
}

/* Register netdevice_init early (subsys = level 4) so that NIC drivers
 * which initialise via device_initcall (level 5) can call netif_register. */
subsys_initcall(netdevice_init);
