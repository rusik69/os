#ifndef AF_PACKET_H
#define AF_PACKET_H

#include "types.h"

/*
 * ── AF_PACKET — Raw packet sockets for Layer 2 access ────────────
 *
 * Provides AF_PACKET / PF_PACKET (domain=17) support with SOCK_RAW
 * for sending and receiving raw Ethernet frames.  Required by ping,
 * tcpdump, DHCP clients, and other network diagnostic tools.
 *
 * Reference: packet(7) man page, Linux net/packet/af_packet.c
 *
 * Socket address structure (struct sockaddr_ll):
 *   sll_family   = AF_PACKET (17)
 *   sll_protocol = Ethernet protocol type (e.g. ETH_P_IP, ETH_P_ALL)
 *   sll_ifindex  = Interface index (0 = any)
 *   sll_hatype   = ARP hardware type (output only)
 *   sll_pkttype  = Packet type (output only)
 *   sll_halen    = Hardware address length
 *   sll_addr     = Hardware address (up to 8 bytes)
 */

/* ── Constants ──────────────────────────────────────────────────── */

#define AF_PACKET           17
#define PF_PACKET           17

/* Packet types (returned in sockaddr_ll.sll_pkttype) */
#define PACKET_HOST         0       /* To us */
#define PACKET_BROADCAST    1       /* To all */
#define PACKET_MULTICAST    2       /* To group */
#define PACKET_OTHERHOST    3       /* To someone else */
#define PACKET_OUTGOING     4       /* Outgoing of any type */

/* Packet socket options (setsockopt) */
#define PACKET_ADD_MEMBERSHIP    1
#define PACKET_DROP_MEMBERSHIP  2
#define PACKET_RX_RING          5
#define PACKET_TX_RING          6
#define PACKET_VERSION          10
#define PACKET_AUXDATA          8
#define PACKET_FANOUT           18
#define PACKET_MR_MULTICAST     0
#define PACKET_MR_PROMISC       1
#define PACKET_MR_ALLMULTI      2
#define PACKET_MR_UNICAST       3

/* Maximum packet socket instances */
#define PACKET_MAX_SOCKETS      16

/* ── Data structures ────────────────────────────────────────────── */

/* AF_PACKET socket address (struct sockaddr_ll) */
struct sockaddr_ll {
    uint16_t sll_family;    /* AF_PACKET (17) */
    uint16_t sll_protocol;  /* Ethernet protocol type (e.g. ETH_P_ALL) */
    int      sll_ifindex;   /* Interface index (0 = any) */
    uint16_t sll_hatype;    /* ARP hardware type (output only) */
    uint8_t  sll_pkttype;   /* Packet type (output only) */
    uint8_t  sll_halen;     /* Hardware address length */
    uint8_t  sll_addr[8];   /* Hardware address (up to 8 bytes) */
} __attribute__((packed));

/* Packet socket filter (BPF) instruction */
struct sock_fprog {
    uint16_t len;       /* Number of filter blocks */
    void    *filter;    /* struct sock_filter[] */
};

/* Packet socket state */
struct packet_sock {
    int         used;           /* 1 = slot in use */
    int         fd;             /* Associated file descriptor */
    uint16_t    protocol;       /* Ethernet proto (ETH_P_ALL = 0x0003) */
    int         ifindex;        /* Bound interface index (0 = any) */
    int         bound;          /* 1 = bound to interface */
    int         promisc;        /* 1 = promiscuous mode */
    int         allmulti;       /* 1 = all multicast */

    /* Packet type filter mask */
    uint32_t    pkttype_mask;   /* Bitmask of PACKET_* types to accept */

    /* Statistics */
    uint64_t    frames_recv;
    uint64_t    frames_sent;
    uint64_t    frames_dropped;
};

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the AF_PACKET subsystem */
void af_packet_init(void);

/* Create a packet socket.  Returns fd on success, -1 on error. */
int  packet_create(int fd, int type, uint16_t protocol);

/* Bind packet socket to an interface by index.  Returns 0 on success, -1 on error. */
int  packet_bind(int fd, int ifindex);

/* Send a raw Ethernet frame.  Returns bytes sent on success, -1 on error. */
int  packet_send(int fd, const void *buf, int len);

/* Receive a raw Ethernet frame.  Returns bytes received on success, -1 on error. */
int  packet_recv(int fd, void *buf, int max_len, uint64_t *src_ifindex);

/* Close a packet socket. */
void packet_close(int fd);

/* Deliver an incoming Ethernet frame to matching packet sockets.
 * Returns the number of sockets that received the frame. */
int  packet_deliver(uint16_t eth_type, int ifindex,
                    const uint8_t *dst_mac, const uint8_t *src_mac,
                    const uint8_t *data, int len);

/* Set/Get socket options on a packet socket. */
int  packet_setsockopt(int fd, int optname, const void *optval, int optlen);
int  packet_getsockopt(int fd, int optname, void *optval, int *optlen);

/* Check if an fd is a packet socket. */
int  packet_is_valid_fd(int fd);

/* Get the bound protocol for an fd (ETH_P_ALL or specific). */
uint16_t packet_get_protocol(int fd);

#endif /* AF_PACKET_H */
