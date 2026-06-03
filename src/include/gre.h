#ifndef GRE_H
#define GRE_H

#include "types.h"

/*
 * GRE (Generic Routing Encapsulation) — RFC 2784 / RFC 2890
 *
 * GRE encapsulates arbitrary network-layer protocol packets inside
 * IP datagrams.  This implementation supports:
 *   - Basic GRE header (no checksum, key, or sequence number)
 *   - IPv4 inner protocol (protocol type 0x0800)
 *   - Tunnel create/destroy lifecycle
 *
 * IP protocol number for GRE: 47
 */

/* ── Constants ─────────────────────────────────────────────────────── */

#define GRE_PROTOCOL        47   /* IP protocol number for GRE */

/* GRE flags (2 bytes at the start of the header) */
#define GRE_FLAG_C          0x8000  /* Checksum present */
#define GRE_FLAG_K          0x2000  /* Key present */
#define GRE_FLAG_S          0x1000  /* Sequence number present */
#define GRE_FLAG_RECURSION  0x0700  /* Recursion control */
#define GRE_FLAG_VERSION    0x0007  /* Version (must be 0) */
#define GRE_VERSION_0       0x0000

/* Protocol types for the GRE header Protocol Type field */
#define GRE_ETH_TYPE_IP     0x0800  /* IPv4 */
#define GRE_ETH_TYPE_IPV6   0x86DD  /* IPv6 (future) */

/* ── GRE header (basic, no options) ───────────────────────────────── */

struct gre_header {
    uint16_t flags;          /* C, K, S, recursion, version */
    uint16_t protocol_type;  /* EtherType of payload (e.g., 0x0800 for IPv4) */
} __attribute__((packed));

/* GRE header with all optional fields (RFC 2890) */
struct gre_full_header {
    uint16_t flags;
    uint16_t protocol_type;
    uint16_t checksum;       /* present when C-bit set */
    uint16_t reserved1;      /* present when C-bit set */
    uint32_t key;            /* present when K-bit set */
    uint32_t seq_num;        /* present when S-bit set */
} __attribute__((packed));

/* ── Tunnel state ──────────────────────────────────────────────────── */

struct gre_tunnel {
    uint32_t remote_ip;      /* Remote tunnel endpoint */
    uint32_t local_ip;       /* Local tunnel endpoint */
    int      active;         /* 1 = tunnel is established */
};

/* ── API ────────────────────────────────────────────────────────────── */

/* Initialize the GRE tunneling subsystem */
int gre_init(void);

/* Shut down the GRE tunneling subsystem */
void gre_exit(void);

/*
 * Create a GRE tunnel between @local and @remote IP addresses.
 * Returns 0 on success, -1 if a tunnel is already active.
 */
int gre_create_tunnel(uint32_t remote, uint32_t local);

/*
 * Destroy the currently active GRE tunnel.
 * Returns 0 on success, -1 if no tunnel is active.
 */
int gre_destroy_tunnel(void);

/*
 * Encapsulate an inner packet inside a GRE+IP outer packet.
 *
 * @inner_pkt   Pointer to the inner network-layer packet (e.g., IP).
 * @inner_len   Length of the inner packet in bytes.
 * @outer_buf   Output buffer for the outer GRE+IP packet.
 * @outer_max   Maximum size of the output buffer.
 *
 * Returns the length of the outer packet on success, or -1 on error.
 */
int gre_encapsulate(const uint8_t *inner_pkt, int inner_len,
                    uint8_t *outer_buf, int outer_max);

/*
 * Decapsulate a GRE+IP outer packet back to the inner packet.
 *
 * @outer_pkt   Pointer to the received outer GRE+IP packet.
 * @outer_len   Length of the outer packet in bytes.
 * @inner_buf   Output buffer for the inner packet.
 * @inner_max   Maximum size of the output buffer.
 *
 * Returns the length of the inner packet on success, or -1 on error.
 */
int gre_decapsulate(const uint8_t *outer_pkt, int outer_len,
                    uint8_t *inner_buf, int inner_max);

#endif /* GRE_H */
