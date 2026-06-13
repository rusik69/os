#ifndef IPOIB_H
#define IPOIB_H

#include "types.h"

/* IP over InfiniBand (IPoIB) — RFC 4391/4392
 * AF_IPOIB = 50 (Linux-compatible pseudo-family)
 */

#define AF_IPOIB            50

/* IPoIB link-layer header (4 bytes) */
struct ipoib_header {
    uint16_t type;      /* Network protocol type (0x0800 = IPv4, 0x86DD = IPv6) */
    uint8_t  reserved[2];
} __attribute__((packed));

#define IPOIB_TYPE_IPV4     0x0800
#define IPOIB_TYPE_IPV6     0x86DD
#define IPOIB_TYPE_ARP      0x0806

/* Broadcast MGID (RFC 4391 Section 4) */
#define IPOIB_BROADCAST_MGID \
    { 0xFF, 0x12, 0x40, 0x1B, 0x00, 0x00, 0x00, 0x00, \
      0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF }

/* IPoIB interface state */
#define IPOIB_MAX_QUEUE     64

struct ipoib_iface {
    int         used;
    int         ifindex;        /* Netdevice index */
    uint8_t     hw_addr[20];    /* 20-byte InfiniBand GID */
    int         mtu;            /* IPoIB MTU (default 2048) */
    /* PKey (partition key) */
    uint16_t    pkey;
    /* Send/Recv queue */
    uint8_t     sndbuf[IPOIB_MAX_QUEUE][2048];
    uint16_t    sndlen[IPOIB_MAX_QUEUE];
    int         snd_head, snd_tail;
    uint8_t     rcvbuf[IPOIB_MAX_QUEUE][2048];
    uint16_t    rcvlen[IPOIB_MAX_QUEUE];
    int         rcv_head, rcv_tail;
    /* Statistics */
    uint64_t    rx_packets, tx_packets;
    uint64_t    rx_bytes, tx_bytes;
    uint64_t    rx_dropped, tx_dropped;
};

/* API */
void ipoib_init(void);
int  ipoib_register_iface(int ifindex, const uint8_t *hw_addr, int mtu);
int  ipoib_unregister_iface(int ifindex);
int  ipoib_send(int ifindex, const uint8_t *dst_gid, uint16_t type,
                const void *data, uint16_t len);
int  ipoib_recv(int ifindex, uint8_t *buf, uint16_t maxlen,
                uint16_t *type_out);
int  ipoib_is_present(void);
void ipoib_poll(void);          /* Process send/receive queues */

#endif /* IPOIB_H */
