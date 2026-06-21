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

/* ── Global registration table ──────────────────────────────────── */

static struct net_device *g_devices[NETDEV_MAX];
static int g_device_count = 0;
static int g_initialized = 0;

/* ── Initialisation ─────────────────────────────────────────────── */

void netdevice_init(void)
{
    if (g_initialized)
        return;
    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
    g_initialized = 1;
    kprintf("[OK] netdevice: initialised (%d slots)\n", NETDEV_MAX);
}

/* ── Registration ───────────────────────────────────────────────── */

int netif_register(struct net_device *dev)
{
    if (!g_initialized) {
        kprintf("[NETDEV] ERROR: netdevice not initialized\n");
        return -1;
    }
    if (!dev) {
        kprintf("[NETDEV] ERROR: NULL device\n");
        return -1;
    }
    if (!dev->transmit) {
        kprintf("[NETDEV] ERROR: device '%s' has no transmit callback\n",
                dev->name[0] ? dev->name : "?");
        return -1;
    }
    if (g_device_count >= NETDEV_MAX) {
        kprintf("[NETDEV] ERROR: device table full (%d max)\n", NETDEV_MAX);
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
        return -1;
    }

    /* Allocate and copy the descriptor */
    struct net_device *copy = (struct net_device *)kmalloc(sizeof(struct net_device));
    if (!copy) {
        kprintf("[NETDEV] ERROR: out of memory for '%s'\n",
                dev->name[0] ? dev->name : "?");
        return -1;
    }
    memcpy(copy, dev, sizeof(struct net_device));
    copy->ifindex = ifindex;

    g_devices[ifindex] = copy;
    g_device_count++;

    kprintf("[NETDEV] registered '%s' (ifindex=%d, mac=%02x:%02x:%02x:%02x:%02x:%02x, mtu=%d)\n",
            copy->name, ifindex,
            copy->mac[0], copy->mac[1], copy->mac[2],
            copy->mac[3], copy->mac[4], copy->mac[5],
            copy->mtu);

    return ifindex;
}

int netif_unregister(int ifindex)
{
    if (!g_initialized)
        return -1;
    if (ifindex < 0 || ifindex >= NETDEV_MAX)
        return -1;
    if (!g_devices[ifindex])
        return -1;

    kprintf("[NETDEV] unregistered '%s' (ifindex=%d)\n",
            g_devices[ifindex]->name, ifindex);

    kfree(g_devices[ifindex]);
    g_devices[ifindex] = NULL;
    g_device_count--;
    return 0;
}

/* ── Transmit / Receive ─────────────────────────────────────────── */

int netif_send(int ifindex, const uint8_t *data, uint16_t len)
{
    if (!g_initialized)
        return -1;
    if (ifindex < 0 || ifindex >= NETDEV_MAX)
        return -1;
    struct net_device *dev = g_devices[ifindex];
    if (!dev || !dev->transmit)
        return -1;

    return dev->transmit(dev, data, len);
}

int netif_recv(int ifindex, uint8_t *buf, uint16_t max_len)
{
    if (!g_initialized)
        return -1;
    if (ifindex < 0 || ifindex >= NETDEV_MAX)
        return -1;
    struct net_device *dev = g_devices[ifindex];
    if (!dev || !dev->receive)
        return -1;

    return dev->receive(dev, buf, max_len);
}

/* ── Lookup ─────────────────────────────────────────────────────── */

struct net_device *netif_get(int ifindex)
{
    if (ifindex < 0 || ifindex >= NETDEV_MAX)
        return NULL;
    return g_devices[ifindex];
}

int netif_name_to_index(const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < NETDEV_MAX; i++) {
        if (g_devices[i] &&
            strcmp(g_devices[i]->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

int netif_count(void)
{
    return g_device_count;
}

/* ── Implement: netdevice_register ────────────────────── */
int netdevice_register(void *dev)
{
    if (!dev) return -EINVAL;
    return netdev_register((struct net_device *)dev);
}
/* ── Implement: netdevice_unregister ──────────────────── */
int netdevice_unregister(void *dev)
{
    if (!dev) return -EINVAL;
    return netdev_unregister((struct net_device *)dev);
}
/* ── Implement: netdevice_open ────────────────────────── */
int netdevice_open(void *dev)
{
    if (!dev) return -EINVAL;
    struct net_device *ndev = (struct net_device *)dev;
    if (ndev->netdev_ops && ndev->netdev_ops->ndo_open)
        return ndev->netdev_ops->ndo_open(ndev);
    return 0;
}
/* ── Implement: netdevice_stop ────────────────────────── */
int netdevice_stop(void *dev)
{
    if (!dev) return -EINVAL;
    struct net_device *ndev = (struct net_device *)dev;
    if (ndev->netdev_ops && ndev->netdev_ops->ndo_stop)
        return ndev->netdev_ops->ndo_stop(ndev);
    return 0;
}
