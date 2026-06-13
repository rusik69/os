#ifndef LOWPAN6_H
#define LOWPAN6_H

#include "types.h"
#include "net.h"

/* 6LoWPAN — IPv6 over Low-Power Wireless Personal Area Networks (RFC 4944/6282)
 * Header compression and adaptation layer for IEEE 802.15.4
 * Operates at layer 2.5, no AF number.
 */

/* IEEE 802.15.4 maximum frame size */
#define IEEE802154_MAX_FRAME    127
#define IEEE802154_MAX_PAYLOAD  102

/* 6LoWPAN dispatch byte values */
#define LOWPAN_DISPATCH_NALP        0x00  /* Not a LoWPAN frame (00xxxxxx) */
#define LOWPAN_DISPATCH_IPV6        0x41  /* Uncompressed IPv6 (01000001) */
#define LOWPAN_DISPATCH_HC1         0x42  /* HC1 compressed (01000010) */
#define LOWPAN_DISPATCH_IPHC        0x60  /* IPHC compressed (011xxxxx) */
#define LOWPAN_DISPATCH_MESH        0x80  /* Mesh header (10xxxxxx) */
#define LOWPAN_DISPATCH_FRAG1       0xC0  /* First fragment (11000xxx) */
#define LOWPAN_DISPATCH_FRAGN       0xE0  /* Subsequent fragment (11100xxx) */

/* IPHC encoding bits */
#define LOWPAN_IPHC_TF              0xC0  /* Traffic Class/Flow Label (2 bits) */
#define LOWPAN_IPHC_NH              0x20  /* Next Header */
#define LOWPAN_IPHC_HLIM            0x18  /* Hop Limit (2 bits) */
#define LOWPAN_IPHC_CID             0x04  /* Context Identifier Extension */
#define LOWPAN_IPHC_SAC             0x02  /* Source Address Compression */
#define LOWPAN_IPHC_SAM             0x01  /* Source Address Mode (combined with SAC) */
/* DAC + DAM for destination (in second byte) */
#define LOWPAN_IPHC_DAC             0x40  /* Dest Address Compression */
#define LOWPAN_IPHC_DAM             0x30  /* Dest Address Mode (combined with DAC) */

/* Hop limit values encoded in HLIM */
#define LOWPAN_HLIM_UNSPEC          0   /* Hop Limit unspecified (inline) */
#define LOWPAN_HLIM_1               1
#define LOWPAN_HLIM_64              2
#define LOWPAN_HLIM_255             3

/* 6LoWPAN context table (for IPHC compression) */
#define LOWPAN_CONTEXT_TABLE_SIZE   16

struct lowpan_context {
    int      used;
    uint8_t  id;                    /* Context ID (0-15) */
    struct in6_addr prefix;         /* Prefix */
    uint8_t  prefix_len;            /* Prefix length in bits */
    uint16_t lifetime;              /* Valid lifetime (minutes) */
};

/* 6LoWPAN frag reassembly state */
#define LOWPAN_FRAG_BUFS 4
#define LOWPAN_FRAG_TIMEOUT 60      /* seconds */

struct lowpan_frag {
    int         used;
    uint16_t    tag;                /* Fragmentation tag */
    uint16_t    size;               /* Datagram size */
    uint16_t    offset;             /* Bytes received so far */
    uint64_t    timeout;            /* Expiry tick */
    uint8_t     buf[IEEE802154_MAX_FRAME * 2]; /* Reassembly buffer */
    uint8_t     src[8];             /* Source address (EUI-64) */
};

/* 6LoWPAN interface state */
struct lowpan_iface {
    int         used;
    int         ifindex;            /* Associated network interface */
    struct lowpan_context ctx[LOWPAN_CONTEXT_TABLE_SIZE];
    struct lowpan_frag frags[LOWPAN_FRAG_BUFS];
    uint16_t    frag_tag_counter;
    uint64_t    rx_packets;
    uint64_t    tx_packets;
    uint64_t    rx_compressed;
    uint64_t    tx_compressed;
    uint64_t    rx_fragments;
    uint64_t    rx_dropped;
};

/* API */
void lowpan6_init(void);
int  lowpan6_register_iface(int ifindex);
int  lowpan6_unregister_iface(int ifindex);
int  lowpan6_compress(const struct ipv6_header *ip6, const void *next_hdr,
                      uint16_t next_len, uint8_t *out, uint16_t *out_len);
int  lowpan6_decompress(const uint8_t *in, uint16_t in_len,
                        struct ipv6_header *ip6, uint8_t *next_hdr_buf,
                        uint16_t *next_len);
int  lowpan6_send(int ifindex, const struct in6_addr *dst,
                   const void *data, uint16_t len);
int  lowpan6_recv(int ifindex, const uint8_t *frame, uint16_t len,
                  struct ipv6_header *ip6, uint8_t *payload, uint16_t *payload_len);
void lowpan6_poll(void);            /* Call from net_poll for frag expiry */

#endif /* LOWPAN6_H */
