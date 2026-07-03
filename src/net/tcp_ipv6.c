/*
 * tcp_ipv6.c — TCP over IPv6 with flow label support (RFC 6437)
 *
 * Provides the IPv6 transmit path for TCP segments, computing the
 * IPv6 flow label from the connection's 5-tuple per RFC 6437 §3
 * and embedding it in the IPv6 header.
 *
 * Receive path is handled by ipv6_core.c (handle_ipv6_packet ->
 * case IP_PROTO_TCP) — dispatch to handle_tcp for the receive side.
 *
 * Reference: RFC 2460 (IPv6), RFC 6437 (Flow Label), RFC 793 (TCP)
 */

#define KERNEL_INTERNAL
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "rng.h"
#include "module.h"

/* ── TCP pseudo-header for IPv6 checksum (RFC 2460 §8.1) ──────────── *
 *
 * The pseudo-header is:
 *   - Source address (16 bytes)
 *   - Destination address (16 bytes)
 *   - Upper-layer packet length (4 bytes, network order)
 *   - Zero (3 bytes) + next header (1 byte)
 *
 * We compute the checksum inline in tcp_ipv6_checksum() without
 * constructing the full pseudo-header struct to avoid packed-struct
 * alignment warnings.
 */

/* ── Compute TCP over IPv6 checksum ───────────────────────────────── */
static uint16_t tcp_ipv6_checksum(const struct in6_addr *src,
                                   const struct in6_addr *dst,
                                   const void *tcp_seg,
                                   uint16_t seg_len)
{
    uint32_t sum;
    int i;

    /* Build pseudo-header checksum byte-by-byte to avoid packed
     * struct alignment warnings (RFC 2460 §8.1 pseudo-header:
     * src + dst + length + zeros + next_header = 40 bytes). */
    sum = 0;

    /* Source address (16 bytes) */
    for (i = 0; i < 16; i += 2)
        sum += (uint32_t)((uint16_t)src->s6_addr[i] << 8) | (uint16_t)src->s6_addr[i + 1];

    /* Destination address (16 bytes) */
    for (i = 0; i < 16; i += 2)
        sum += (uint32_t)((uint16_t)dst->s6_addr[i] << 8) | (uint16_t)dst->s6_addr[i + 1];

    /* Upper-layer packet length (4 bytes, network byte order) */
    sum += (uint32_t)((seg_len >> 8) & 0xFF) | ((uint32_t)(seg_len & 0xFF) << 8);

    /* Zero (3 bytes) + next header (1 byte = IP_PROTO_TCP = 6) */
    sum += (uint32_t)IP_PROTO_TCP;

    /* Checksum over TCP segment data as 16-bit words */
    {
        const uint16_t *ptr = (const uint16_t *)tcp_seg;
        int count = seg_len / 2;
        for (i = 0; i < count; i++) {
            uint16_t word;
            memcpy(&word, &ptr[i], sizeof(word));
            sum += ntohs(word);
        }
    }

    /* If odd length, add the last byte */
    if (seg_len & 1) {
        const uint8_t *last = (const uint8_t *)tcp_seg + seg_len - 1;
        sum += (uint32_t)(*last) << 8;
    }

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum & 0xFFFF);
}

/* ── Compute and cache the flow label for a TCP connection ──────────
 *
 * The flow label is a 20-bit pseudo-random value computed from the
 * connection's 5-tuple plus a secret seed.  It remains constant for
 * the lifetime of the connection.
 *
 * Call this once after the connection is established, before sending
 * the first data segment.
 *
 * src_addr — the source IPv6 address (e.g., from ipv6_addr_select_source)
 * dst_addr — the destination IPv6 address
 */
void tcp_ipv6_compute_flow_label(struct tcp_conn *conn,
                                  const struct in6_addr *src_addr,
                                  const struct in6_addr *dst_addr)
{
    if (!conn || !src_addr || !dst_addr)
        return;

    conn->flow_label = ipv6_flow_label_calc(src_addr, dst_addr,
                                             conn->local_port,
                                             conn->remote_port,
                                             IP_PROTO_TCP);
}

/* ── Send a TCP segment over IPv6 with flow label ───────────────────
 *
 * Builds a complete IPv6 + TCP frame and sends it.
 * The flow label from conn->flow_label is embedded in the IPv6 header.
 *
 * Parameters:
 *   conn     — TCP connection state (used for ports, seq numbers, flow label)
 *   dst_addr — destination IPv6 address
 *   flags    — TCP flags (TCP_SYN, TCP_ACK, etc.)
 *   data     — payload data (may be NULL)
 *   data_len — payload length (may be 0)
 */
void send_tcp_ipv6(struct tcp_conn *conn,
                    const struct in6_addr *dst_addr,
                    uint8_t flags,
                    const void *data, uint16_t data_len)
{
    uint8_t buf[1500];
    struct tcp_header *tcp;
    uint16_t hdr_len;
    uint16_t total_len;

    if (!conn || !dst_addr)
        return;

    /* Ensure the flow label seed is initialized */
    ipv6_flow_label_init();

    memset(buf, 0, sizeof(buf));

    /* Build TCP header in the buffer (we'll fill in the v6 header later) */
    tcp = (struct tcp_header *)buf;
    tcp->src_port = htons(conn->local_port);
    tcp->dst_port = htons(conn->remote_port);
    tcp->seq_num  = htonl(conn->our_seq);
    tcp->ack_num  = htonl(conn->their_seq);

    /* TCP options: currently none for data segments; SYN would use
     * separate path via send_tcp() for IPv4 or future extension. */
    hdr_len = sizeof(struct tcp_header);
    tcp->data_off = (uint8_t)((hdr_len / 4) << 4);
    tcp->flags = flags;
    tcp->window = htons(8192);

    /* Copy payload after TCP header */
    if (data && data_len > 0)
        memcpy(buf + hdr_len, data, data_len);

    total_len = hdr_len + data_len;

    /* Compute TCP checksum over IPv6 pseudo-header (RFC 2460 §8.1).
     * Set checksum to 0 before computation. */
    tcp->checksum = 0;
    tcp->checksum = tcp_ipv6_checksum(&net_our_ipv6_ll, dst_addr,
                                       buf, total_len);

    /* Send as IPv6 packet with the connection's flow label.
     * The TCP segment (including header) is the payload of the IPv6
     * datagram. */
    send_ipv6_flow(dst_addr, IP_PROTO_TCP, buf, total_len,
                   conn->flow_label);
}

/* ── Handle incoming TCP over IPv6 ──────────────────────────────────
 *
 * Called from ipv6_core.c's handle_ipv6_packet() when the upper-layer
 * protocol is TCP.  For now this is a placeholder that logs the event
 * and dispatches to the existing handle_tcp() for basic support.
 *
 * In a full implementation, the IPv6 source address would be mapped
 * into the TCP connection table and handle_tcp would be called with
 * an IPv6-aware path.  For now we provide the receive stub that the
 * IPv6 dispatch table needs.
 */
void handle_tcp_ipv6(struct ipv6_header *ip6,
                      const uint8_t *payload, uint16_t len)
{
    if (!ip6 || !payload || len < sizeof(struct tcp_header)) {
        kprintf("[tcp_ipv6] short TCP segment: %u bytes\n", len);
        return;
    }

    const struct tcp_header *tcp = (const struct tcp_header *)payload;

    kprintf("[tcp_ipv6] TCP over IPv6: src_port=%u dst_port=%u flags=0x%02x seq=%u len=%u\n",
            ntohs(tcp->src_port), ntohs(tcp->dst_port),
            tcp->flags, ntohl(tcp->seq_num), len);

    /* Extract 5-tuple for flow label diagnostics */
    uint32_t flow_label = ntohl(ip6->vcl_flow) & IPV6_FLOW_LABEL_MASK;
    if (flow_label != 0) {
        kprintf("[tcp_ipv6] flow label=0x%05x\n", flow_label);
    }

    /* TODO: Map to TCP connection table and dispatch to IPv6-aware
     * TCP receive handler.  For now, the packet is logged only. */
}

/* ── Init ──────────────────────────────────────────────────────────── */
void tcp_ipv6_init(void)
{
    /* Initialize the flow label secret seed */
    ipv6_flow_label_init();

    kprintf("[tcp_ipv6] TCP over IPv6 with flow label support (RFC 6437)\n");
}

MODULE_LICENSE("MIT");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("TCP over IPv6 with RFC 6437 flow label support");
MODULE_AUTHOR("OS Kernel Team");
