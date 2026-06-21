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

#endif /* AF_PACKET_H */
