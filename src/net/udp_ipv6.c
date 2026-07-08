/*
 * udp_ipv6.c — UDP over IPv6 with mandatory checksum (RFC 2460 §8.1, RFC 768)
 *
 * In IPv6, the UDP checksum is MANDATORY (unlike IPv4 where it is optional).
 * Per RFC 2460 §8.1, the pseudo-header includes the source and destination
 * IPv6 addresses, the upper-layer packet length, zeros, and the next-header
 * value (17 for UDP).
 *
 * Provides:
 *   - send_udp_ipv6()      — send a UDP datagram over IPv6
 *   - handle_udp_ipv6()    — receive and dispatch UDP over IPv6
 *
 * Reference:
 *   RFC 768   — User Datagram Protocol
 *   RFC 2460  — Internet Protocol, Version 6 (IPv6) Specification
 *   RFC 8200  — Internet Protocol, Version 6 (IPv6) Specification (obsoletes 2460)
 */

#define KERNEL_INTERNAL
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "module.h"

/*
 * ── UDP over IPv6 checksum (RFC 2460 §8.1 + RFC 768) ───────────────
 *
 * The checksum is computed over a pseudo-header followed by the UDP
 * header and data.  The pseudo-header is:
 *
 *   +-----------+-----------+-----------+-----------+
 *   |                    src addr                   |
 *   +-----------+-----------+-----------+-----------+
 *   |                    dst addr                   |
 *   +-----------+-----------+-----------+-----------+
 *   |          UDP length          |    zeros   | NH|
 *   +-----------+-----------+-----------+-----------+
 *
 * NH = next header (17 = IP_PROTO_UDP)
 * UDP length = sizeof(struct udp_header) + data_len (in network byte order)
 *
 * We delegate to the generic ipv6_checksum() which already implements
 * the IPv6 pseudo-header checksum per RFC 8200 §8.1.
 */

/*
 * ── Send a UDP datagram over IPv6 ───────────────────────────────────
 *
 * Builds a complete IPv6 + UDP frame with the mandatory checksum and
 * sends it via send_ipv6().
 *
 * Parameters:
 *   dst      — destination IPv6 address
 *   src_port — source UDP port (host byte order)
 *   dst_port — destination UDP port (host byte order)
 *   data     — UDP payload (may be NULL if data_len == 0)
 *   data_len — length of payload
 */
void send_udp_ipv6(const struct in6_addr *dst,
                   uint16_t src_port, uint16_t dst_port,
                   const void *data, uint16_t data_len)
{
    uint8_t buf[1500];
    struct udp_header *udp;
    uint16_t udp_len;
    uint16_t total;

    if (!dst)
        return;

    memset(buf, 0, sizeof(buf));

    /* Build UDP header at the start of the buffer */
    udp = (struct udp_header *)buf;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp_len = sizeof(struct udp_header) + data_len;
    udp->length = htons(udp_len);

    /* Copy payload after UDP header */
    if (data && data_len > 0)
        memcpy(buf + sizeof(struct udp_header), data, data_len);

    /*
     * Compute the IPv6 pseudo-header checksum.
     * For IPv6, UDP checksum is MANDATORY (RFC 2460 §8.1).
     *
     * We need the source address for the pseudo-header.  Since
     * send_ipv6() will select the source address internally, we
     * use the same source selection logic to get the address for
     * the checksum.
     */
    {
        struct ipv6_addr_entry *src_entry;
        struct in6_addr src_addr;
        int have_src = 0;

        src_entry = ipv6_addr_select_source(dst);
        if (src_entry) {
            memcpy(&src_addr, &src_entry->addr, sizeof(struct in6_addr));
            have_src = 1;
        } else if (net_ipv6_ll_ready) {
            memcpy(&src_addr, &net_our_ipv6_ll, sizeof(struct in6_addr));
            have_src = 1;
        }

        if (have_src) {
            /* Set checksum to 0 before computation (RFC 768) */
            udp->checksum = 0;
            udp->checksum = ipv6_checksum(&src_addr, dst,
                                           IP_PROTO_UDP,
                                           buf, udp_len);
        } else {
            /*
             * No IPv6 address configured — set checksum to 0.
             * Note: RFC 2460 §8.1 requires the checksum to be
             * non-zero for IPv6.  However, transmitting without
             * a source address is already broken, so this is a
             * last-resort edge case.
             */
            udp->checksum = 0;
        }
    }

    total = udp_len;

    /* Send as IPv6 packet (send_ipv6 will select the source address) */
    send_ipv6(dst, IP_PROTO_UDP, buf, total);
}

/*
 * ── Handle incoming UDP over IPv6 ───────────────────────────────────
 *
 * Called from ipv6_core.c's handle_ipv6_packet() when the upper-layer
 * protocol is UDP.  Validates the UDP header, verifies the mandatory
 * IPv6 pseudo-header checksum, and dispatches to registered UDP
 * handlers or logs the packet.
 *
 * For now, we log the event and attempt to dispatch via the existing
 * (IPv4) UDP binding table.  A full IPv6-aware UDP binding system
 * would use the source IPv6 address for dispatch; this implementation
 * provides the plumbing placeholder.
 *
 * Parameters:
 *   ip6     — the received IPv6 header
 *   payload — pointer to the UDP datagram (UDP header + data)
 *   len     — total length of the UDP datagram (including UDP header)
 */
void handle_udp_ipv6(struct ipv6_header *ip6,
                      const uint8_t *payload, uint16_t len)
{
    struct udp_header *udp;
    uint16_t dst_port;
    uint16_t src_port;
    uint16_t udp_len;
    uint16_t data_len;
    uint16_t saved_checksum;
    uint16_t computed_checksum;

    if (!ip6 || !payload) {
        kprintf("[udp_ipv6] NULL pointer in handle_udp_ipv6\n");
        return;
    }

    if (len < sizeof(struct udp_header)) {
        kprintf("[udp_ipv6] short UDP segment: %u bytes\n", len);
        net_iface_stats.rx_errors++;
        return;
    }

    udp = (struct udp_header *)payload;
    dst_port = ntohs(udp->dst_port);
    src_port = ntohs(udp->src_port);
    udp_len = ntohs(udp->length);

    /* Validate the UDP length field */
    if (udp_len < sizeof(struct udp_header) || udp_len > len) {
        kprintf("[udp_ipv6] invalid UDP length: header=%u actual=%u\n",
                udp_len, len);
        net_iface_stats.rx_errors++;
        return;
    }

    data_len = (uint16_t)(udp_len - sizeof(struct udp_header));

    /*
     * Verify the mandatory IPv6 pseudo-header checksum.
     * For IPv6, UDP checksum MUST be present and correct.
     *
     * Per RFC 2460 §8.1, a receiver MUST discard a UDP packet
     * with a zero checksum (it is not optional like in IPv4).
     */
    if (udp->checksum == 0) {
        kprintf("[udp_ipv6] zero checksum on UDP over IPv6 — "
                "discarding (RFC 2460 §8.1)\n");
        net_iface_stats.rx_errors++;
        return;
    }

    /* Save and clear checksum before computation (RFC 768) */
    saved_checksum = udp->checksum;
    udp->checksum = 0;

    computed_checksum = ipv6_checksum(&ip6->src_ip, &ip6->dst_ip,
                                       IP_PROTO_UDP,
                                       payload, udp_len);

    if (computed_checksum != saved_checksum) {
        kprintf("[udp_ipv6] checksum mismatch: computed=0x%04x "
                "received=0x%04x (port %u)\n",
                computed_checksum, saved_checksum, dst_port);
        net_iface_stats.rx_errors++;
        /* Restore checksum for debugging */
        udp->checksum = saved_checksum;
        return;
    }

    /* Restore checksum for potential downstream use */
    udp->checksum = saved_checksum;

    kprintf("[udp_ipv6] UDP over IPv6: src_port=%u dst_port=%u "
            "data_len=%u checksum=0x%04x\n",
            src_port, dst_port, data_len, saved_checksum);

    const uint8_t *udp_data = payload + sizeof(struct udp_header);

    /*
     * Try to dispatch via the existing (IPv4) UDP binding table.
     * The handler signature uses uint32_t for source IP, so we pass
     * the lower 32 bits of the IPv6 source address as an approximation.
     *
     * TODO: Implement a proper IPv6-aware UDP receive/dispatch system
     * that carries the full source IPv6 address to the handler.
     */
    {
        int dispatched = 0;
        int i;

        for (i = 0; i < net_num_udp_bindings; i++) {
            if (net_udp_bindings[i].port == dst_port &&
                net_udp_bindings[i].handler) {
                /*
                 * Extract lower 32 bits of IPv6 source as a
                 * best-effort IPv4-compatible address for the
                 * existing handler API.
                 */
                uint32_t src_ip_v4 =
                    ((uint32_t)ip6->src_ip.s6_addr[12] << 24) |
                    ((uint32_t)ip6->src_ip.s6_addr[13] << 16) |
                    ((uint32_t)ip6->src_ip.s6_addr[14] << 8)  |
                    (uint32_t)ip6->src_ip.s6_addr[15];

                net_udp_bindings[i].handler(src_ip_v4, src_port,
                                            udp_data, data_len);
                dispatched = 1;
                break;
            }
        }

        if (!dispatched) {
            kprintf("[udp_ipv6] no handler for port %u\n", dst_port);
            /* TODO: Send ICMPv6 Destination Unreachable (port unreachable) */
        }
    }
}

/*
 * ── Init ────────────────────────────────────────────────────────────
 */
void udp_ipv6_init(void)
{
    kprintf("[udp_ipv6] UDP over IPv6 with mandatory checksum (RFC 2460 §8.1)\n");
}

MODULE_LICENSE("MIT");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("UDP over IPv6 with mandatory checksum (RFC 2460 §8.1)");
MODULE_AUTHOR("OS Kernel Team");
