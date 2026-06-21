#ifndef AF_PACKET_H
#define AF_PACKET_H

#include "types.h"

/* ── Packet socket constants (AF_PACKET, AF_UNSPEC) ───────────── */

#define AF_PACKET    17

/* Packet types for sll_pkttype */
#define PACKET_HOST       0      /* To us */
#define PACKET_BROADCAST  1      /* To all */
#define PACKET_MULTICAST  2      /* To group */
#define PACKET_OTHERHOST  3      /* To someone else */
#define PACKET_OUTGOING   4      /* Outgoing of any type */

/* Packet socket options (SOL_PACKET level) */
#define PACKET_ADD_MEMBERSHIP   1
#define PACKET_DROP_MEMBERSHIP  2
#define PACKET_RECV_TYPE        3
#define PACKET_VERSION          4
#define PACKET_AUXDATA          8
#define PACKET_FANOUT           18
#define PACKET_TX_HIGH_TSO      21
#define PACKET_RX_RING          5
#define PACKET_TX_RING          13
#define PACKET_MR_ALLMULTI      3

/* Packet socket filter (BPF instruction set) */
#define BPF_CLASS(code)  ((code) & 0x07)
#define BPF_LD    0x00
#define BPF_LDX   0x01
#define BPF_ST    0x02
#define BPF_STX   0x03
#define BPF_ALU   0x04
#define BPF_JMP   0x05
#define BPF_RET   0x06
#define BPF_MISC  0x07

#define BPF_SIZE(code)  ((code) & 0x18)
#define BPF_W     0x00
#define BPF_H     0x08
#define BPF_B     0x10

#define BPF_MODE(code)  ((code) & 0xe0)
#define BPF_IMM   0x00
#define BPF_ABS   0x20
#define BPF_IND   0x40
#define BPF_MEM   0x60
#define BPF_LEN   0x80

/* BPF ALU operations */
#define BPF_ADD   0x00
#define BPF_SUB   0x10
#define BPF_MUL   0x20
#define BPF_DIV   0x30
#define BPF_OR    0x40
#define BPF_AND   0x50
#define BPF_LSH   0x60
#define BPF_RSH   0x70
#define BPF_NEG   0x80
#define BPF_MOD   0x90
#define BPF_XOR   0xa0

/* BPF jump conditions */
#define BPF_JA    0x00
#define BPF_JEQ   0x10
#define BPF_JGT   0x20
#define BPF_JGE   0x30
#define BPF_JSET  0x40

/* BPF source (k = constant, x = X register) */
#define BPF_K     0x00
#define BPF_X     0x08

/* BPF miscellaneous */
#define BPF_TAX   0x00
#define BPF_TXA   0x80

/* BPF instruction structure */
struct sock_filter {
    unsigned short code;
    unsigned char  jt;
    unsigned char  jf;
    unsigned int   k;
};

/* sock_fprog for attaching a packet filter */
struct sock_fprog {
    unsigned short len;
    struct sock_filter *filter;
};

/* Packet socket address structure */
struct sockaddr_ll {
    unsigned short sll_family;    /* AF_PACKET */
    unsigned short sll_protocol;  /* ETH_P_* */
    int            sll_ifindex;   /* Interface index */
    unsigned short sll_hatype;    /* ARP hardware type */
    unsigned char  sll_pkttype;   /* PACKET_* */
    unsigned char  sll_halen;     /* Length of sll_addr */
    unsigned char  sll_addr[8];   /* Physical layer address */
};

/* Packet filter statistics / info */
struct tpacket_stats {
    unsigned int tp_packets;
    unsigned int tp_drops;
};

/* packet_mreq for multicast groups */
struct packet_mreq {
    int            mr_ifindex;
    unsigned short mr_type;
    unsigned short mr_alen;
    unsigned char  mr_address[8];
};

/* ── Packet socket internals (for af_packet.c) ──────────────── */

#define PACKET_MAX_SOCKETS 64

/* TP_STATUS flags for mmap ring */
#define TP_STATUS_KERNEL        0x00
#define TP_STATUS_USER          0x01
#define TP_STATUS_COPY          0x02
#define TP_STATUS_LOSING        0x04
#define TP_STATUS_SENDING       0x08
#define TP_STATUS_SENDFAIL      0x10

/* tpacket header for mmap ring */
struct tpacket_hdr {
    unsigned long  tp_status;
    unsigned int   tp_len;
    unsigned int   tp_snaplen;
    unsigned short tp_mac;
    unsigned short tp_net;
    unsigned int   tp_sec;
    unsigned int   tp_usec;
};

/* tpacket_req for PACKET_RX_RING / PACKET_TX_RING */
struct tpacket_req {
    unsigned int tp_block_size;
    unsigned int tp_block_nr;
    unsigned int tp_frame_size;
    unsigned int tp_frame_nr;
};

/* Packet mmap ring buffer */
struct packet_mmap_ring {
    int                     active;
    uint8_t                *base;
    uint32_t                block_size;
    uint32_t                block_nr;
    uint32_t                frame_size;
    uint32_t                frame_nr;
    uint32_t                frames_per_block;
    uint32_t                frame_count;
    uint32_t                last_frame;
    uint64_t                pg_vec_addr;
    uint32_t                pg_vec_len;
    volatile struct tpacket_hdr **frames;
};

/* Multicast/bind entry */
struct packet_mc_entry {
    struct packet_mc_entry *next;
    uint8_t                  mr_address[8];
    int                      mr_ifindex;
    int                      mr_type;
    unsigned short           mr_alen;
};

/* BPF return values */
#define BPF_PASS      1   /* accept packet */
#define BPF_KILL      0   /* drop packet */

/* Packet socket structure */
struct packet_sock {
    int     used;
    int     fd;
    int     bound;
    int     ifindex;
    int     protocol;
    uint32_t pkttype_mask;
    uint32_t frames_recv;
    int     mmap_enabled;
    int     auxdata_enabled;
    struct packet_mmap_ring mmap_ring;
    struct sock_filter     *filter_prog;
    int     filter_len;
    int     filter_active;
    struct packet_mc_entry *mc_list;
    int     allmulti;
};

/* ── AF_PACKET API ────────────────────────────────────────── */
void af_packet_init(void);
int  packet_create(int fd, int type, uint16_t protocol);
int  packet_bind(int fd, int ifindex);
void packet_close(int fd);
int  packet_is_valid_fd(int fd);
int  packet_send(int fd, const void *buf, int len);
int  packet_recv(int fd, void *buf, int bufsize, uint64_t *ifindex);
int  packet_getsockname(int fd, struct sockaddr_ll *addr);

#endif /* AF_PACKET_H */
