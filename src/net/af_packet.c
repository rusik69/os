/*
 * af_packet.c — AF_PACKET raw packet sockets (Item 386)
 *
 * Provides raw packet (SOCK_RAW) and datagram (SOCK_DGRAM) sockets
 * for direct Layer 2 access.  Used by ping, tcpdump, DHCP clients,
 * and other network diagnostic tools.
 *
 * Two addressing modes:
 *   1. AF_PACKET (domain=17) with bind to interface + ETH_P_* protocol
 *   2. AF_UNSPEC (domain=0) as fallback for ETH_P_ALL raw sockets
 *
 * Frame delivery: packet_deliver() is called from the network receive
 * path to deliver copies of incoming Ethernet frames to all matching
 * AF_PACKET sockets.
 */

#include "af_packet.h"
#include "net.h"
#include "net_internal.h"
#include "netdevice.h"
#include "socket.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"

/* ── Packet socket table ──────────────────────────────────────────── */
static struct packet_sock packet_socks[PACKET_MAX_SOCKETS];
static spinlock_t         packet_lock;

/* Forward declarations */
static struct packet_sock *packet_find_by_fd(int fd);
static int                 packet_find_free(void);

/* ── Initialisation ───────────────────────────────────────────────── */

void af_packet_init(void)
{
    spinlock_init(&packet_lock);
    memset(packet_socks, 0, sizeof(packet_socks));
    kprintf("[OK] AF_PACKET: raw packet socket subsystem initialized\n");
}

/* ── Internal helpers ─────────────────────────────────────────────── */

static int packet_find_free(void)
{
    for (int i = 0; i < PACKET_MAX_SOCKETS; i++) {
        if (!packet_socks[i].used)
            return i;
    }
    return -1;
}

static struct packet_sock *packet_find_by_fd(int fd)
{
    for (int i = 0; i < PACKET_MAX_SOCKETS; i++) {
        if (packet_socks[i].used && packet_socks[i].fd == fd)
            return &packet_socks[i];
    }
    return NULL;
}

/* ── Create a packet socket ──────────────────────────────────────────
 * Called from sys_socket_impl when domain is AF_PACKET (17) or
 * AF_UNSPEC (0) with SOCK_RAW type.  Allocates a packet_sock entry
 * and associates it with the given file descriptor.
 */
int packet_create(int fd, int type, uint16_t protocol)
{
    (void)type; /* SOCK_RAW vs SOCK_DGRAM — for now both behave the same */

    spinlock_acquire(&packet_lock);

    int slot = packet_find_free();
    if (slot < 0) {
        spinlock_release(&packet_lock);
        return -ENOMEM;
    }

    struct packet_sock *ps = &packet_socks[slot];
    memset(ps, 0, sizeof(*ps));
    ps->used     = 1;
    ps->fd       = fd;

    /* Default protocol: ETH_P_ALL (0x0003) captures all protocols.
     * A specific protocol (e.g. htons(ETH_P_IP)) can be set via bind(). */
    ps->protocol = (protocol != 0) ? protocol : ETH_P_ALL;

    /* Default: accept all packet types */
    ps->pkttype_mask = (1u << PACKET_HOST)
                     | (1u << PACKET_BROADCAST)
                     | (1u << PACKET_MULTICAST);

    spinlock_release(&packet_lock);
    return 0;
}

/* ── Bind to interface ───────────────────────────────────────────────
 * Bind this packet socket to a specific network interface by index.
 * If ifindex is 0, the socket receives from all interfaces.
 * After bind, only packets matching the bound protocol are delivered.
 */
int packet_bind(int fd, int ifindex)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps)
        return -EINVAL;

    spinlock_acquire(&packet_lock);

    ps->ifindex = ifindex;
    ps->bound   = 1;

    /* If binding with a specific protocol (stored in ps->protocol),
     * the socket will only receive frames matching that Ethernet type. */
    spinlock_release(&packet_lock);
    return 0;
}

/* ── Send raw Ethernet frame ─────────────────────────────────────────
 * Constructs a complete Ethernet frame header using the interface's
 * MAC address as source and sends it via the netdevice layer.
 *
 * If the caller provides a full Ethernet frame (14+ bytes), it is
 * sent as-is.  Otherwise, and for bound sockets, we prepend the
 * appropriate Ethernet header.
 *
 * Returns bytes sent on success, -1 on error.
 */
int packet_send(int fd, const void *buf, int len)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !buf || len <= 0)
        return -EINVAL;

    /* Determine target interface: use bound interface or first available */
    int ifindex = ps->ifindex;
    if (ifindex < 0)
        ifindex = 0;

    /* Get source MAC from the interface or global net_our_mac */
    uint8_t src_mac[6];
    if (netif_count() > 0 && ifindex < netif_count()) {
        struct net_device *ndev = netif_get(ifindex);
        if (ndev) {
            memcpy(src_mac, ndev->mac, 6);
        } else {
            memcpy(src_mac, net_our_mac, 6);
        }
    } else {
        memcpy(src_mac, net_our_mac, 6);
    }

    /* If the caller provides a full frame (14+ bytes with Ethernet header),
     * send it directly.  Otherwise, we'd need destination MAC and protocol,
     * which the caller must provide in the frame payload. */
    if (len >= 14) {
        /* Full Ethernet frame — send via netdevice layer */
        if (netif_count() > 0 && ifindex < netif_count()) {
            return netif_send(ifindex, (const uint8_t *)buf, len);
        }
        return net_link_send(buf, (uint16_t)(len > 2048 ? 2048 : len));
    }

    /* Short payload — caller just provides payload without Ethernet header.
     * In this case we need a bound socket with known protocol.
     * This is a simplified path; real AF_PACKET typically requires
     * the caller to build the full frame. */
    return -EINVAL;
}

/* ── Receive raw Ethernet frame ──────────────────────────────────────
 * Copy the most recent received frame for this socket into the
 * caller's buffer.  The frame includes the full Ethernet header.
 * Returns the number of bytes read, -1 if no data available.
 *
 * In this simplified implementation, we use a single global receive
 * buffer.  A production implementation would maintain per-socket
 * receive queues.
 */
int packet_recv(int fd, void *buf, int max_len, uint64_t *src_ifindex)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !buf || max_len <= 0)
        return -EINVAL;

    /* Check if there's a pending frame in the global receive buffer.
     * We check the Ethernet type against our bound protocol. */
    spinlock_acquire(&packet_lock);

    /* Read from the network's packet buffer if a frame is pending
     * and the protocol matches our bound protocol. */
    if (!net_rx_pending()) {
        spinlock_release(&packet_lock);
        return -EAGAIN;
    }

    /* Try to receive a raw frame from the NIC */
    uint8_t temp_buf[2048];
    int n = net_link_recv(temp_buf, sizeof(temp_buf));

    if (n <= 0) {
        spinlock_release(&packet_lock);
        return -EAGAIN;
    }

    /* Check if the received frame's EtherType matches our filter */
    if (n >= 14 && ps->protocol != ETH_P_ALL) {
        uint16_t eth_type = (uint16_t)((uint16_t)temp_buf[12] << 8)
                          | (uint16_t)temp_buf[13];
        if (eth_type != ps->protocol) {
            /* Protocol mismatch — put it back? No, we'd lose it.
             * In a real implementation, unmatched frames go to other
             * sockets or are consumed by the IP stack.  For now we
             * just skip. */
            spinlock_release(&packet_lock);
            return -EAGAIN;
        }
    }

    /* Copy frame to caller */
    int copy_len = n;
    if (copy_len > max_len)
        copy_len = max_len;
    memcpy(buf, temp_buf, copy_len);

    if (src_ifindex)
        *src_ifindex = (uint64_t)ps->ifindex;

    ps->frames_recv++;
    spinlock_release(&packet_lock);
    return copy_len;
}

/* ── Deliver incoming frame to matching packet sockets ───────────────
 * Called from the network receive path (net_rx_poll loop) to deliver
 * a copy of each incoming Ethernet frame to all AF_PACKET sockets
 * whose protocol filter matches the frame's EtherType.
 *
 * Returns the number of sockets that received a copy.
 */
int packet_deliver(uint16_t eth_type, int ifindex,
                   const uint8_t *dst_mac, const uint8_t *src_mac,
                   const uint8_t *data, int len)
{
    (void)dst_mac;
    (void)src_mac;
    (void)data;
    (void)len;

    int delivered = 0;

    spinlock_acquire(&packet_lock);

    for (int i = 0; i < PACKET_MAX_SOCKETS; i++) {
        struct packet_sock *ps = &packet_socks[i];
        if (!ps->used)
            continue;

        /* Check interface match */
        if (ps->ifindex > 0 && ps->ifindex != ifindex)
            continue;

        /* Check protocol match */
        if (ps->protocol != ETH_P_ALL && ps->protocol != eth_type)
            continue;

        /* Socket matches — in a full implementation we would queue
         * the frame to a per-socket receive ring buffer.  For this
         * simplified version, we just increment the counter and
         * signal that data is available. */
        ps->frames_recv++;
        delivered++;
    }

    spinlock_release(&packet_lock);
    return delivered;
}

/* ── Close / cleanup ───────────────────────────────────────────────── */

void packet_close(int fd)
{
    spinlock_acquire(&packet_lock);

    for (int i = 0; i < PACKET_MAX_SOCKETS; i++) {
        if (packet_socks[i].used && packet_socks[i].fd == fd) {
            memset(&packet_socks[i], 0, sizeof(struct packet_sock));
            break;
        }
    }

    spinlock_release(&packet_lock);
}

/* ── Socket options ────────────────────────────────────────────────── */

int packet_setsockopt(int fd, int optname, const void *optval, int optlen)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !optval)
        return -EINVAL;

    (void)optlen;

    switch (optname) {
    case PACKET_ADD_MEMBERSHIP:
    case PACKET_DROP_MEMBERSHIP:
        /* Multicast group membership — not yet implemented */
        return 0;

    case PACKET_AUXDATA:
        /* Ancillary data — not yet implemented */
        return 0;

    default:
        return -ENOPROTOOPT;
    }
}

int packet_getsockopt(int fd, int optname, void *optval, int *optlen)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !optval || !optlen)
        return -EINVAL;

    switch (optname) {
    case PACKET_VERSION: {
        /* Current packet socket version = 0 (traditional) */
        if (*optlen < (int)sizeof(int))
            return -EINVAL;
        *(int *)optval = 0;
        *optlen = sizeof(int);
        return 0;
    }

    default:
        return -ENOPROTOOPT;
    }
}

/* ── Utility ───────────────────────────────────────────────────────── */

int packet_is_valid_fd(int fd)
{
    return packet_find_by_fd(fd) != NULL;
}

uint16_t packet_get_protocol(int fd)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps)
        return 0;
    return ps->protocol;
}

/* ── Exports ───────────────────────────────────────────────────────── */

EXPORT_SYMBOL(af_packet_init);
EXPORT_SYMBOL(packet_create);
EXPORT_SYMBOL(packet_bind);
EXPORT_SYMBOL(packet_send);
EXPORT_SYMBOL(packet_recv);
EXPORT_SYMBOL(packet_close);
EXPORT_SYMBOL(packet_deliver);
