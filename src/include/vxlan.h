#ifndef VXLAN_H
#define VXLAN_H

#include "types.h"

/*
 * VXLAN (Virtual eXtensible Local Area Network) — RFC 7348
 *
 * VXLAN encapsulates L2 Ethernet frames inside UDP datagrams for
 * multi-tenant overlay networking.  Each tunnel is identified by a
 * 24-bit VXLAN Network Identifier (VNI), allowing up to 16 million
 * isolated layer-2 segments over a shared IP infrastructure.
 *
 * The outer encapsulation uses:
 *   - Outer Ethernet header (14 bytes)
 *   - Outer IPv4 header (20 bytes, protocol = UDP = 17)
 *   - Outer UDP header (8 bytes, dst port = IANA 8472)
 *   - VXLAN header (8 bytes)
 *   - Inner Ethernet frame (payload)
 *
 * This implementation supports:
 *   - VXLAN tunnel creation/destruction
 *   - Encapsulation of Ethernet frames into VXLAN+UDP+IPv4 packets
 *   - Decapsulation of VXLAN+UDP+IPv4 back to Ethernet frames
 *   - Multiple VNI-based tunnels
 *   - Loadable kernel module support via MODULE guards
 */

/* ── Constants ─────────────────────────────────────────────────────── */

#define VXLAN_UDP_PORT      8472   /* IANA-assigned VXLAN destination port */
#define VXLAN_PROTO_UDP     17     /* IP protocol number for UDP */

/* VXLAN header flags: the VNI flag (bit 3 = 0x08) MUST be set for
 * valid VXLAN Network ID frames per RFC 7348 section 5. */
#define VXLAN_FLAG_VNI      0x08   /* VNI present flag */

/* Maximum number of simultaneous VXLAN tunnels */
#define VXLAN_MAX_TUNNELS   8

/* ── VXLAN header (8 bytes) ──────────────────────────────────────────
 *
 *   Flags (8 bits) | Reserved (24 bits) | VNI (24 bits) | Reserved (8 bits)
 *
 * All multi-byte fields are in network byte order (big-endian).
 */
struct vxlan_header {
    uint32_t flags_vni;  /* Upper 8 bits = flags, next 24 bits = reserved,
                          * lower 24 bits = VNI (MSB-aligned) */
} __attribute__((packed));

#define VXLAN_HDR_LEN  sizeof(struct vxlan_header)

/* ── Tunnel state ──────────────────────────────────────────────────── */

struct vxlan_tunnel {
    uint32_t remote_ip;      /* Remote tunnel endpoint (network byte order) */
    uint32_t local_ip;       /* Local tunnel endpoint (network byte order) */
    uint16_t src_port;       /* Local UDP source port (derived from VNI hash) */
    uint32_t vni;            /* VXLAN Network Identifier (24 bits) */
    int      active;         /* 1 = tunnel is established */
};

/* ── API ────────────────────────────────────────────────────────────── */

/* Initialize the VXLAN tunneling subsystem */
int vxlan_init(void);

/* Shut down the VXLAN tunneling subsystem */
void vxlan_exit(void);

/*
 * Create a VXLAN tunnel.
 *
 * @remote  Remote endpoint IP address (network byte order).
 * @local   Local endpoint IP address (network byte order).
 * @vni     24-bit VXLAN Network Identifier.
 *
 * Returns 0 on success, -1 if no free tunnel slot or invalid params.
 */
int vxlan_create_tunnel(uint32_t remote, uint32_t local, uint32_t vni);

/*
 * Destroy a VXLAN tunnel identified by its VNI.
 *
 * @vni  The VXLAN Network Identifier of the tunnel to destroy.
 *
 * Returns 0 on success, -1 if no tunnel with that VNI is found.
 */
int vxlan_destroy_tunnel(uint32_t vni);

/*
 * Encapsulate an inner Ethernet frame inside a VXLAN+UDP+IPv4 packet.
 *
 * @inner_eth  Pointer to the inner Ethernet frame (including MAC header).
 * @inner_len  Length of the inner Ethernet frame in bytes.
 * @outer_buf  Output buffer for the outer IP packet (without Ethernet header).
 * @outer_max  Maximum size of the output buffer.
 * @vni        VXLAN Network Identifier (24 bits).
 *
 * Returns the length of the outer packet on success, or -1 on error.
 * The outer_buf contains an IPv4 header + UDP header + VXLAN header +
 * inner Ethernet frame.
 */
int vxlan_encapsulate(const uint8_t *inner_eth, int inner_len,
                      uint8_t *outer_buf, int outer_max, uint32_t vni);

/*
 * Decapsulate a received VXLAN+UDP+IPv4 packet back to the inner
 * Ethernet frame.
 *
 * @outer_pkt  Pointer to the received outer IP packet (starting at IPv4 hdr).
 * @outer_len  Length of the outer packet in bytes.
 * @inner_buf  Output buffer for the inner Ethernet frame.
 * @inner_max  Maximum size of the output buffer.
 * @out_vni    Receives the VNI from the VXLAN header (24 bits).
 *
 * Returns the length of the inner Ethernet frame on success, or -1 on
 * error (e.g., bad VXLAN header, UDP port mismatch, checksum error).
 */
int vxlan_decapsulate(const uint8_t *outer_pkt, int outer_len,
                      uint8_t *inner_buf, int inner_max,
                      uint32_t *out_vni);

/*
 * Look up the tunnel state for a given VNI.
 *
 * @vni  The VXLAN Network Identifier.
 *
 * Returns a pointer to the tunnel state, or NULL if not found.
 */
struct vxlan_tunnel *vxlan_tunnel_lookup(uint32_t vni);

#endif /* VXLAN_H */
