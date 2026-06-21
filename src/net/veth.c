/*
 * veth.c — Virtual Ethernet pair driver
 *
 * Creates paired virtual Ethernet interfaces.  Each endpoint is a
 * standard net_device that can be used with the bridge, routing,
 * or connected to a network namespace.
 *
 * Transmit on one endpoint enqueues the Ethernet frame into the
 * peer endpoint's receive ring buffer.  The peer's receive callback
 * dequeues from that buffer.  This provides a simple, zero-copy-avoiding
 * (but correct) virtual wire between the two ends.
 */

#define KERNEL_INTERNAL
#include "veth.h"
#include "netdevice.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "export.h"

/* ── Global state ───────────────────────────────────────────────── */

static struct veth_endpoint veth_endpoints[VETH_MAX_PAIRS * 2];
static int veth_endpoint_count = 0;
static int veth_initialized = 0;

/* Spinlock for ring-buffer operations (brief, no contention expected) */
static volatile int veth_lock = 0;

static inline void veth_lock_acquire(void) {
    while (__sync_lock_test_and_set(&veth_lock, 1))
        __asm__ volatile("pause");
    __sync_synchronize();
}

static inline void veth_lock_release(void) {
    __sync_synchronize();
    __sync_lock_release(&veth_lock);
}

/* ── MAC address generation ─────────────────────────────────────── */

/*
 * Generate a locally-administered MAC address.
 * The first octet has bit 1 set (locally administered) and bit 0 clear
 * (unicast).  The remaining 5 octets come from a simple LCG so that
 * each veth pair gets a unique address.
 */
static uint64_t veth_mac_seed = 0x123456789ABCDEF0ULL;

static void veth_gen_mac(uint8_t mac[6])
{
    veth_mac_seed = veth_mac_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    mac[0] = 0x02;  /* locally administered, unicast */
    mac[1] = (uint8_t)((veth_mac_seed >> 40) & 0xFF);
    mac[2] = (uint8_t)((veth_mac_seed >> 32) & 0xFF);
    mac[3] = (uint8_t)((veth_mac_seed >> 24) & 0xFF);
    mac[4] = (uint8_t)((veth_mac_seed >> 16) & 0xFF);
    mac[5] = (uint8_t)((veth_mac_seed >>  8) & 0xFF);
}

/* ── Endpoint lookup ────────────────────────────────────────────── */

/* Find a veth endpoint by its net_device ifindex.
 * Returns NULL if not found or not a veth device. */
static struct veth_endpoint *veth_find_by_ifindex(int ifindex)
{
    if (ifindex < 0) return NULL;
    for (int i = 0; i < veth_endpoint_count; i++) {
        if (veth_endpoints[i].active &&
            veth_endpoints[i].ifindex == ifindex)
            return &veth_endpoints[i];
    }
    return NULL;
}

/* ── Net device callbacks ───────────────────────────────────────── */

/*
 * Transmit callback: called when a frame is sent on this endpoint.
 * We enqueue the frame into the peer's receive ring.
 */
static int veth_transmit(struct net_device *dev,
                          const uint8_t *data, uint16_t len)
{
    if (!dev || !data || len == 0 || len > VETH_MTU + 14)
        return -1;

    struct veth_endpoint *ep = (struct veth_endpoint *)dev->priv;
    if (!ep || !ep->active)
        return -1;

    /* Find the peer endpoint */
    struct veth_endpoint *peer = veth_find_by_ifindex(ep->peer_ifindex);
    if (!peer || !peer->active)
        return -1;  /* peer gone */

    veth_lock_acquire();

    /* Check if the ring has space */
    if (peer->ring_count >= VETH_RING_SIZE) {
        /* Ring full — drop the packet */
        veth_lock_release();
        return -1;
    }

    /* Copy the frame into the peer's receive ring */
    struct veth_ring_slot *slot = &peer->ring[peer->ring_tail];
    memcpy(slot->data, data, len);
    slot->len = len;
    slot->occupied = 1;

    peer->ring_tail = (peer->ring_tail + 1) % VETH_RING_SIZE;
    peer->ring_count++;

    veth_lock_release();

    return 0;
}

/*
 * Receive callback: called to poll for a received frame.
 * We dequeue from our own receive ring.
 */
static int veth_receive(struct net_device *dev,
                         uint8_t *buf, uint16_t max_len)
{
    if (!dev || !buf || max_len == 0)
        return -1;

    struct veth_endpoint *ep = (struct veth_endpoint *)dev->priv;
    if (!ep || !ep->active)
        return -1;

    veth_lock_acquire();

    if (ep->ring_count == 0) {
        veth_lock_release();
        return 0;  /* nothing available */
    }

    /* Dequeue the oldest frame */
    struct veth_ring_slot *slot = &ep->ring[ep->ring_head];
    uint16_t copy_len = slot->len < max_len ? slot->len : max_len;
    memcpy(buf, slot->data, copy_len);
    slot->occupied = 0;

    ep->ring_head = (ep->ring_head + 1) % VETH_RING_SIZE;
    ep->ring_count--;

    veth_lock_release();

    return (int)copy_len;
}

/* ── Create / Destroy ───────────────────────────────────────────── */

int veth_create_pair(const char *name1, const char *name2,
                      int out_ifindex[2])
{
    if (!veth_initialized) {
        kprintf("[VETH] ERROR: veth not initialized\\n");
        return -1;
    }
    if (!name1 || !name2 || name1[0] == '\0' || name2[0] == '\0') {
        kprintf("[VETH] ERROR: invalid names\\n");
        return -1;
    }
    if (veth_endpoint_count + 2 > VETH_MAX_PAIRS * 2) {
        kprintf("[VETH] ERROR: max pairs reached (%d)\\n", VETH_MAX_PAIRS);
        return -1;
    }

    /* Check for duplicate names */
    if (netif_name_to_index(name1) >= 0 ||
        netif_name_to_index(name2) >= 0) {
        kprintf("[VETH] ERROR: interface name already in use\\n");
        return -1;
    }

    /* Allocate two endpoint slots */
    int idx_a = veth_endpoint_count;
    int idx_b = veth_endpoint_count + 1;

    struct veth_endpoint *ep_a = &veth_endpoints[idx_a];
    struct veth_endpoint *ep_b = &veth_endpoints[idx_b];

    memset(ep_a, 0, sizeof(*ep_a));
    memset(ep_b, 0, sizeof(*ep_b));

    /* Generate MAC addresses */
    veth_gen_mac(ep_a->mac);
    veth_gen_mac(ep_b->mac);

    /* Set up endpoint A */
    int len = (int)strlen(name1);
    if (len >= (int)sizeof(ep_a->name)) len = (int)sizeof(ep_a->name) - 1;
    memcpy(ep_a->name, name1, (size_t)len);
    ep_a->name[len] = '\0';
    ep_a->active = 1;

    /* Set up endpoint B */
    len = (int)strlen(name2);
    if (len >= (int)sizeof(ep_b->name)) len = (int)sizeof(ep_b->name) - 1;
    memcpy(ep_b->name, name2, (size_t)len);
    ep_b->name[len] = '\0';
    ep_b->active = 1;

    /* Register endpoint A as a net_device */
    struct net_device nd_a;
    memset(&nd_a, 0, sizeof(nd_a));
    memcpy(nd_a.name, ep_a->name, sizeof(nd_a.name));
    memcpy(nd_a.mac, ep_a->mac, 6);
    nd_a.transmit = veth_transmit;
    nd_a.receive  = veth_receive;
    nd_a.mtu      = VETH_MTU;
    nd_a.flags    = 1;  /* IFF_UP */
    nd_a.priv     = (void *)ep_a;

    int if_a = netif_register(&nd_a);
    if (if_a < 0) {
        ep_a->active = 0;
        ep_b->active = 0;
        kprintf("[VETH] ERROR: failed to register '%s'\\n", name1);
        return -1;
    }
    ep_a->ifindex = if_a;

    /* Register endpoint B as a net_device */
    struct net_device nd_b;
    memset(&nd_b, 0, sizeof(nd_b));
    memcpy(nd_b.name, ep_b->name, sizeof(nd_b.name));
    memcpy(nd_b.mac, ep_b->mac, 6);
    nd_b.transmit = veth_transmit;
    nd_b.receive  = veth_receive;
    nd_b.mtu      = VETH_MTU;
    nd_b.flags    = 1;  /* IFF_UP */
    nd_b.priv     = (void *)ep_b;

    int if_b = netif_register(&nd_b);
    if (if_b < 0) {
        /* Roll back: unregister A */
        netif_unregister(if_a);
        ep_a->active = 0;
        ep_b->active = 0;
        kprintf("[VETH] ERROR: failed to register '%s'\\n", name2);
        return -1;
    }
    ep_b->ifindex = if_b;

    /* Wire the peer pointers */
    ep_a->peer_ifindex = if_b;
    ep_b->peer_ifindex = if_a;

    veth_endpoint_count += 2;

    if (out_ifindex) {
        out_ifindex[0] = if_a;
        out_ifindex[1] = if_b;
    }

    kprintf("[VETH] pair created: %s (ifindex=%d) <-> %s (ifindex=%d)\\n",
            name1, if_a, name2, if_b);

    return 0;
}

int veth_destroy(int ifindex)
{
    if (!veth_initialized)
        return -1;

    struct veth_endpoint *ep = veth_find_by_ifindex(ifindex);
    if (!ep) {
        kprintf("[VETH] ERROR: no veth device at ifindex %d\\n", ifindex);
        return -1;
    }

    struct veth_endpoint *peer = veth_find_by_ifindex(ep->peer_ifindex);

    /* Unregister both ends from the netdevice layer */
    if (peer) {
        int peer_if = peer->ifindex;
        peer->active = 0;
        netif_unregister(peer_if);
    }

    int ep_if = ep->ifindex;
    ep->active = 0;
    netif_unregister(ep_if);

    kprintf("[VETH] pair destroyed: %s (ifindex=%d) <-> %s\\n",
            ep->name, ep_if,
            peer ? peer->name : "(unknown)");

    return 0;
}

/* ── Initialization ─────────────────────────────────────────────── */

void veth_init(void)
{
    if (veth_initialized)
        return;

    memset(veth_endpoints, 0, sizeof(veth_endpoints));
    veth_endpoint_count = 0;
    veth_initialized = 1;

    kprintf("[OK] veth: virtual ethernet pair driver initialized "
            "(max %d pairs, ring %d slots)\\n",
            VETH_MAX_PAIRS, VETH_RING_SIZE);
}

/* ── Exported symbols for loadable kernel modules ────────────────── */
EXPORT_SYMBOL(veth_create_pair);
EXPORT_SYMBOL(veth_destroy);

/* ── Stub: veth_open ─────────────────────────────── */
int veth_open(void *dev)
{
    (void)dev;
    kprintf("[veth] veth_open: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: veth_stop ─────────────────────────────── */
int veth_stop(void *dev)
{
    (void)dev;
    kprintf("[veth] veth_stop: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: veth_xmit ─────────────────────────────── */
int veth_xmit(void *skb, void *dev)
{
    (void)skb;
    (void)dev;
    kprintf("[veth] veth_xmit: not yet implemented\n");
    return -ENOSYS;
}
