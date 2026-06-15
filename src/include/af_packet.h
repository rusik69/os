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

/* ── PACKET_MMAP ring (tpacket) ─────────────────────────────────── */

/* tpacket ring block size (must be page-aligned, default 4096) */
#define TPACKET_ALIGNMENT       16
#define TPACKET_ALIGN(x)       (((x) + TPACKET_ALIGNMENT - 1) & ~(TPACKET_ALIGNMENT - 1))

/* tpacket block descriptor for PACKET_MMAP */
struct tpacket_hdr {
    uint64_t tp_status;         /* Status: TP_STATUS_KERNEL / TP_STATUS_USER */
    uint32_t tp_len;            /* Frame length */
    uint32_t tp_snaplen;        /* Captured length */
    uint16_t tp_mac;            /* Offset to MAC header */
    uint16_t tp_net;            /* Offset to network header */
    uint32_t tp_sec;            /* Seconds (timestamp) */
    uint32_t tp_nsec;           /* Nanoseconds (timestamp) */
    uint16_t tp_vlan_tci;
    uint16_t tp_vlan_tpid;
} __attribute__((packed));

#define TP_STATUS_KERNEL        0       /* Kernel owns the frame */
#define TP_STATUS_USER          1       /* User owns the frame */
#define TP_STATUS_COPY          2
#define TP_STATUS_VLAN_VALID    4
#define TP_STATUS_BLK_TMO       8

/* PACKET_MMAP ring configuration */
struct tpacket_req {
    uint32_t tp_block_size;     /* Size of each block */
    uint32_t tp_block_nr;       /* Number of blocks */
    uint32_t tp_frame_size;     /* Size of each frame */
    uint32_t tp_frame_nr;       /* Number of frames */
};

/* Per-socket PACKET_MMAP ring state */
struct packet_mmap_ring {
    int              active;            /* 1 = ring is set up */
    uint8_t         *base;              /* Virtual base address */
    uint32_t         block_size;
    uint32_t         block_nr;
    uint32_t         frame_size;
    uint32_t         frame_nr;
    uint32_t         frames_per_block;
    volatile struct tpacket_hdr **frames; /* Array of frame pointers */
    uint32_t         frame_count;       /* Total frames in ring */
    uint32_t         last_frame;        /* Last frame processed */
    uint64_t         pg_vec_addr;       /* Physical page vector address */
    uint32_t         pg_vec_len;        /* Number of pages in vector */
};

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
struct sock_filter {
    uint16_t code;      /* Filter opcode */
    uint8_t  jt;        /* Jump true offset */
    uint8_t  jf;        /* Jump false offset */
    uint32_t k;         /* Generic multi-use field */
};

/* Packet socket filter program */
struct sock_fprog {
    uint16_t           len;       /* Number of filter blocks */
    struct sock_filter *filter;  /* Pointer to struct sock_filter[] */
};

/* BPF return values */
#define BPF_KILL          0
#define BPF_PASS          1

/* Multicast group membership request (for PACKET_ADD_MEMBERSHIP) */
struct packet_mreq {
    int            mr_ifindex;
    uint16_t       mr_type;
    uint16_t       mr_alen;
    uint8_t        mr_address[8];
};

/* Packet ancillary data header (tpkt_auxdata) */
struct tpacket_auxdata {
    uint32_t tp_status;
    uint32_t tp_len;
    uint32_t tp_snaplen;
    uint16_t tp_mac;
    uint16_t tp_net;
    uint32_t tp_sec;
    uint32_t tp_nsec;
    uint16_t tp_vlan_tci;
    uint16_t tp_vlan_tpid;
};

/* Multicast group membership entry */
struct packet_mc_entry {
    struct packet_mc_entry *next;
    int         mr_ifindex;
    uint16_t    mr_type;        /* PACKET_MR_MULTICAST, etc */
    uint8_t     mr_alen;
    uint8_t     mr_address[8];
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

    /* Ancillary data (PACKET_AUXDATA) */
    int         auxdata_enabled; /* 1 = deliver tpkt_auxdata on recvmsg */

    /* Multicast group membership list */
    struct packet_mc_entry *mc_list;  /* linked list of mc addresses */

    /* Statistics */
    uint64_t    frames_recv;
    uint64_t    frames_sent;
    uint64_t    frames_dropped;

    /* PACKET_MMAP ring */
    struct packet_mmap_ring mmap_ring;
    int mmap_enabled;           /* 1 = mmap ring active */
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

/* Get socket name (for getsockname).  Returns 0 or -errno. */
int  packet_getsockname(int fd, struct sockaddr_ll *addr);

/* Simple BPF filter support.  Returns 0 on success, -errno on error. */
int  packet_set_filter(int fd, const struct sock_fprog *fprog);
int  packet_apply_filter(int fd, const uint8_t *frame, int len);

/* Check if an fd is a packet socket. */
int  packet_is_valid_fd(int fd);

/* Get the bound protocol for an fd (ETH_P_ALL or specific). */
uint16_t packet_get_protocol(int fd);

/* PACKET_MMAP ring operations */
int  packet_mmap_setup(int fd, struct tpacket_req *req, int tx_ring);
int  packet_mmap_poll(int fd, int rx);
int  packet_mmap_get_frame(int fd, int rx, uint32_t *frame_id);
int  packet_mmap_release_frame(int fd, int rx, uint32_t frame_id);
void packet_mmap_teardown(int fd);

#endif /* AF_PACKET_H */
