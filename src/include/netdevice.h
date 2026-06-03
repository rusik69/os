#ifndef NETDEVICE_H
#define NETDEVICE_H

#include "types.h"

/*
 * netdevice.h — Network device (netdev) interface layer
 *
 * Provides a registration-based abstraction over physical/virtual NICs.
 * Each driver (e1000, virtio-net, etc.) registers itself as a net device
 * with a name, MAC address, and transmit callback.  The bridge and other
 * subsystems can then send frames on a specific interface by index.
 *
 * Architecture:
 *   netif_register(driver_device)   — called during NIC init
 *   netif_send(ifindex, data, len)  — transmit on a specific interface
 *   netif_name(ifindex)             — get interface name for debugging
 *   netif_count()                   — number of registered interfaces
 */

/* Maximum number of net devices supported */
#define NETDEV_MAX 8

/* Maximum interface name length (including NUL) */
#define NETDEV_NAME_MAX 16

/* Forward declaration */
struct net_device;

/* Transmit callback: send @len bytes starting at @data on this interface.
 * Must return 0 on success, -1 on failure.  The callback is responsible
 * for any necessary locking (the caller holds no device-specific lock). */
typedef int (*netdev_tx_fn)(struct net_device *dev,
                            const uint8_t *data, uint16_t len);

/* Receive callback: poll for a received packet.
 * Returns the number of bytes received (0 if none), or -1 on error.
 * The driver copies the received Ethernet frame into @buf (max @max_len). */
typedef int (*netdev_rx_fn)(struct net_device *dev,
                            uint8_t *buf, uint16_t max_len);

/* Network device descriptor.
 * Drivers fill in the fields and call netif_register(). */
struct net_device {
    char          name[NETDEV_NAME_MAX];   /* e.g. "eth0", "virtio0" */
    uint8_t       mac[6];                  /* MAC address */
    int           ifindex;                 /* assigned by netif_register (>=0) */
    netdev_tx_fn  transmit;                /* send a frame (must be set) */
    netdev_rx_fn  receive;                 /* poll for received frame (or NULL) */
    int           mtu;                     /* maximum transmission unit */
    int           flags;                   /* IFF_UP, etc. */
    void         *priv;                    /* driver-private data */
};

/* ── API ────────────────────────────────────────────────────────── */

/* Initialise the netdevice subsystem.  Called once at boot. */
void netdevice_init(void);

/* Register a network device with the system.
 * Copies the descriptor; sets dev->ifindex to the assigned index.
 * Returns the interface index (>= 0) on success, -1 on failure. */
int netif_register(struct net_device *dev);

/* Unregister a previously registered network device by its index.
 * Returns 0 on success, -1 if no device is registered at @ifindex. */
int netif_unregister(int ifindex);

/* Transmit an Ethernet frame on the interface identified by @ifindex.
 * @data points to the complete frame (including Ethernet header).
 * Returns 0 on success, -1 if the interface index is invalid. */
int netif_send(int ifindex, const uint8_t *data, uint16_t len);

/* Receive a packet from the interface @ifindex (non-blocking poll).
 * Copies the received frame into @buf (max @max_len bytes).
 * Returns the number of bytes received, 0 if nothing available,
 * or -1 on error / invalid interface. */
int netif_recv(int ifindex, uint8_t *buf, uint16_t max_len);

/* Look up a network device by its index.
 * Returns a pointer to the registered net_device, or NULL.
 * The returned pointer is valid until netif_unregister is called. */
struct net_device *netif_get(int ifindex);

/* Find the index of the first interface whose name matches @name.
 * Returns the ifindex (>= 0) or -1 if not found. */
int netif_name_to_index(const char *name);

/* Return the number of currently registered interfaces. */
int netif_count(void);

/* Check whether the given ifindex is valid (in range and registered). */
static inline int netif_valid(int ifindex) {
    return (ifindex >= 0 && ifindex < NETDEV_MAX &&
            netif_get(ifindex) != NULL);
}

#endif /* NETDEVICE_H */
