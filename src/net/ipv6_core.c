/*
 * ipv6_core.c — IPv6 core functions: extension header parsing, packet dispatch
 *
 * Implements RFC 8200 §4 (Extension Headers) including:
 *  - Hop-by-Hop Options Header (next header 0)
 *  - Routing Header (next header 43)
 *  - Fragment Header (next header 44) — detection for reassembly
 *  - Authentication Header (next header 51)
 *  - Destination Options Header (next header 60)
 *
 * The main entry point is handle_ipv6_packet(), which walks the extension
 * header chain and dispatches the final payload to the appropriate
 * upper-layer protocol handler (ICMPv6, TCP, UDP).
 */

#define KERNEL_INTERNAL
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "timer.h"

/*
 * ── Extension header walker ───────────────────────────────────────
 *
 * Given the IPv6 header and total frame length, walk the extension header
 * chain (RFC 8200 §4.1) and return:
 *   - *out_proto:    the next-header value of the first non-extension header
 *   - *out_payload:  pointer to the first byte of the upper-layer payload
 *   - *out_payload_len: remaining bytes after all extension headers
 *   - *out_frag_hdr: if a Fragment Header was encountered, pointer to it
 *                     (NULL otherwise) — caller can check for reassembly
 *
 * Returns 0 on success, negative errno on parse error.
 *
 * Extension headers (in order they MUST appear per RFC 8200 §4.1):
 *   0  = Hop-by-Hop Options
 *   43 = Routing
 *   44 = Fragment
 *   50 = ESP
 *   51 = Authentication
 *   60 = Destination Options (before upper-layer when preceded by Routing,
 *        or before ESP when used for per-packet options)
 *
 * For simplicity we accept any order and simply skip known extension
 * headers in a single-pass loop.  Unknown next-header values terminate
 * the chain and are returned as the final protocol.
 */

static int ipv6_is_extension_header(uint8_t nh)
{
    switch (nh) {
    case IPV6_NEXTHDR_HOPOPT:    /* 0 */
    case IPV6_NEXTHDR_ROUTING:   /* 43 */
    case IPV6_NEXTHDR_FRAGMENT:  /* 44 */
    case IPV6_NEXTHDR_ESP:       /* 50 */
    case IPV6_NEXTHDR_AUTH:      /* 51 */
    case IPV6_NEXTHDR_DEST:      /* 60 */
        return 1;
    default:
        return 0;
    }
}

/*
 * ipv6_parse_exthdr — walk the extension header chain
 *
 * Parameters:
 *   data         — pointer to the start of the IPv6 header
 *   total_len    — total frame length (from Ethernet header)
 *   out_proto    — [out] final protocol number (next header of first
 *                  non-extension header)
 *   out_payload  — [out] pointer to the upper-layer payload
 *   out_payload_len — [out] length of the upper-layer payload
 *   out_frag_hdr — [out] pointer to Fragment Header if found, else NULL
 *
 * Returns 0 on success, negative errno on error.
 */
int ipv6_parse_exthdr(const uint8_t *data, uint16_t total_len,
                       uint8_t *out_proto,
                       const uint8_t **out_payload,
                       uint16_t *out_payload_len,
                       const struct ipv6_fragment **out_frag_hdr)
{
    if (!data || !out_proto || !out_payload || !out_payload_len) {
        return -EINVAL;
    }

    /* Must at least have the IPv6 header */
    if (total_len < sizeof(struct ipv6_header)) {
        kprintf("[ipv6_core] packet too short: %u < %zu\n",
                total_len, sizeof(struct ipv6_header));
        return -EINVAL;
    }

    const struct ipv6_header *ip6 = (const struct ipv6_header *)data;
    uint16_t payload_len = ntohs(ip6->payload_length);

    /* Sanity check: payload_length + header must match the frame */
    if ((uint32_t)sizeof(struct ipv6_header) + (uint32_t)payload_len > (uint32_t)total_len) {
        kprintf("[ipv6_core] payload_length mismatch: hdr=%zu payload=%u total=%u\n",
                sizeof(struct ipv6_header), payload_len, total_len);
        return -EINVAL;
    }

    /* If payload_length is 0 (jumbo gram), we can't parse further */
    if (payload_len == 0) {
        *out_proto = ip6->next_header;
        *out_payload = NULL;
        *out_payload_len = 0;
        if (out_frag_hdr) *out_frag_hdr = NULL;
        return 0;
    }

    uint8_t  next_hdr = ip6->next_header;
    const uint8_t *ptr  = data + sizeof(struct ipv6_header);
    uint16_t remaining = payload_len;
    const struct ipv6_fragment *frag_hdr = NULL;

    /* Walk the extension header chain */
    while (remaining > 0 && ipv6_is_extension_header(next_hdr)) {
        switch (next_hdr) {
        case IPV6_NEXTHDR_HOPOPT:   /* 0 — Hop-by-Hop Options */
        case IPV6_NEXTHDR_DEST: {   /* 60 — Destination Options */
            if (remaining < sizeof(struct ipv6_exthdr_opt)) {
                kprintf("[ipv6_core] truncated options header (nh=%u, rem=%u)\n",
                        next_hdr, remaining);
                return -EINVAL;
            }
            const struct ipv6_exthdr_opt *opt =
                (const struct ipv6_exthdr_opt *)ptr;
            uint16_t hdr_len = (uint16_t)(opt->hdr_ext_len + 1) * 8;
            if (hdr_len > remaining || hdr_len < sizeof(struct ipv6_exthdr_opt)) {
                kprintf("[ipv6_core] invalid options header len: %u (rem=%u)\n",
                        hdr_len, remaining);
                return -EINVAL;
            }
            /* Process known TLV options for Hop-by-Hop */
            if (next_hdr == IPV6_NEXTHDR_HOPOPT && hdr_len > sizeof(struct ipv6_exthdr_opt)) {
                /* Walk TLV options in this header */
                const uint8_t *opt_ptr = ptr + sizeof(struct ipv6_exthdr_opt);
                uint16_t opt_remaining = (uint16_t)(hdr_len - sizeof(struct ipv6_exthdr_opt));
                while (opt_remaining > 0) {
                    uint8_t opt_type = opt_ptr[0];
                    if (opt_type == IPV6_TLV_PAD1) {
                        /* Pad1 — 1 byte, no len/data */
                        opt_ptr += 1;
                        opt_remaining -= 1;
                        continue;
                    }
                    /* All other options have at least type + len */
                    if (opt_remaining < 2) break;
                    uint8_t opt_len = opt_ptr[1];
                    uint16_t opt_total = (uint16_t)opt_len + 2;
                    if (opt_total > opt_remaining) break;

                    /* Known options we care about */
                    switch (opt_type) {
                    case IPV6_TLV_ROUTER_ALERT:
                        if (opt_len >= 2) {
                            uint16_t ra_val = (uint16_t)opt_ptr[2] << 8 |
                                               (uint16_t)opt_ptr[3];
                            (void)ra_val;
                            kprintf("[ipv6_core] Router Alert option: %u\n", ra_val);
                        }
                        break;
                    case IPV6_TLV_JUMBO:
                        if (opt_len >= 4) {
                            uint32_t jumbo_payload =
                                (uint32_t)opt_ptr[2] << 24 |
                                (uint32_t)opt_ptr[3] << 16 |
                                (uint32_t)opt_ptr[4] << 8  |
                                (uint32_t)opt_ptr[5];
                            kprintf("[ipv6_core] Jumbo Payload: %u bytes\n",
                                    jumbo_payload);
                        }
                        break;
                    default:
                        /* Unknown option — check action bits (upper 2 bits) */
                        uint8_t action = (opt_type >> 6) & 0x03;
                        if (action == 0x02) {
                            /* Discard the packet, skip further processing */
                            kprintf("[ipv6_core] unrecognized option type 0x%02x: discarding\n",
                                    opt_type);
                            return -EINVAL;
                        }
                        break;
                    }
                    opt_ptr += opt_total;
                    opt_remaining -= opt_total;
                }
            }
            next_hdr = opt->next_header;
            ptr += hdr_len;
            remaining -= hdr_len;
            break;
        }

        case IPV6_NEXTHDR_ROUTING: { /* 43 — Routing Header */
            if (remaining < sizeof(struct ipv6_routing)) {
                kprintf("[ipv6_core] truncated routing header (rem=%u)\n", remaining);
                return -EINVAL;
            }
            const struct ipv6_routing *rh = (const struct ipv6_routing *)ptr;
            uint16_t hdr_len = (uint16_t)(rh->hdr_ext_len + 1) * 8;
            if (hdr_len > remaining || hdr_len < sizeof(struct ipv6_routing)) {
                kprintf("[ipv6_core] invalid routing header len: %u (rem=%u)\n",
                        hdr_len, remaining);
                return -EINVAL;
            }
            /* Type 0 (deprecated) carries strict/loose source route;
             * Type 2 (MIPv6) carries home address.
             * We simply skip routing headers for now. */
            (void)rh->routing_type;
            (void)rh->segments_left;
            next_hdr = rh->next_header;
            ptr += hdr_len;
            remaining -= hdr_len;
            break;
        }

        case IPV6_NEXTHDR_FRAGMENT: { /* 44 — Fragment Header */
            if (remaining < sizeof(struct ipv6_fragment)) {
                kprintf("[ipv6_core] truncated fragment header (rem=%u)\n", remaining);
                return -EINVAL;
            }
            const struct ipv6_fragment *fh = (const struct ipv6_fragment *)ptr;
            frag_hdr = fh;  /* record for caller */

            uint16_t frag_off = IPV6_FRAG_OFFSET(fh);
            int more = IPV6_FRAG_MORE(fh);

            if (frag_off != 0 || more) {
                /* Fragmented packet — we don't reassemble yet but we
                 * log it and continue parsing.  The caller can check
                 * out_frag_hdr to decide whether to reassemble. */
                kprintf("[ipv6_core] fragmented IPv6 packet: offset=%u more=%u id=%u\n",
                        frag_off, more, ntohl(fh->identification));
            }
            next_hdr = fh->next_header;
            ptr += sizeof(struct ipv6_fragment);
            remaining -= (uint16_t)sizeof(struct ipv6_fragment);
            break;
        }

        case IPV6_NEXTHDR_ESP: { /* 50 — ESP */
            /* ESP has variable length.  The payload_len field is in
             * 4-byte units minus 2.  We skip ESP by computing the
             * full header size and moving past the ICV. */
            if (remaining < sizeof(struct ipv6_auth)) {
                kprintf("[ipv6_core] truncated ESP header (rem=%u)\n", remaining);
                return -EINVAL;
            }
            const struct ipv6_auth *esp = (const struct ipv6_auth *)ptr;
            uint16_t hdr_len = (uint16_t)(esp->payload_len + 2) * 4;
            if (hdr_len > remaining) {
                kprintf("[ipv6_core] invalid ESP header len: %u (rem=%u)\n",
                        hdr_len, remaining);
                return -EINVAL;
            }
            kprintf("[ipv6_core] ESP header: spi=0x%08x seq=%u\n",
                    ntohl(esp->spi), ntohl(esp->sequence_number));
            next_hdr = esp->next_header;
            ptr += hdr_len;
            remaining -= hdr_len;
            break;
        }

        case IPV6_NEXTHDR_AUTH: { /* 51 — Authentication Header */
            if (remaining < sizeof(struct ipv6_auth)) {
                kprintf("[ipv6_core] truncated AH (rem=%u)\n", remaining);
                return -EINVAL;
            }
            const struct ipv6_auth *ah = (const struct ipv6_auth *)ptr;
            uint16_t hdr_len = (uint16_t)(ah->payload_len + 2) * 4;
            if (hdr_len > remaining) {
                kprintf("[ipv6_core] invalid AH len: %u (rem=%u)\n",
                        hdr_len, remaining);
                return -EINVAL;
            }
            next_hdr = ah->next_header;
            ptr += hdr_len;
            remaining -= hdr_len;
            break;
        }

        default:
            /* Unknown extension header — treat as final protocol */
            goto done;
        }
    }

done:
    *out_proto = next_hdr;
    *out_payload = ptr;
    *out_payload_len = remaining;
    if (out_frag_hdr) *out_frag_hdr = frag_hdr;
    return 0;
}

/*
 * ── Fragment reassembly (RFC 8200 §4.5) ─────────────────────────────
 *
 * IPv6 fragmentation on receive: when a Fragment Header is present with
 * offset > 0 or More Fragments flag set, the packet is one of several
 * fragments.  We reassemble them in a slot keyed by
 * (src_addr, dst_addr, identification).
 *
 * Atomic fragments (offset=0, M=0) are not actually fragmented — they
 * pass through to the upper-layer handler as-is.
 */

struct ipv6_frag_slot {
    struct in6_addr src;                       /* source address */
    struct in6_addr dst;                       /* destination address */
    uint32_t        identification;            /* IP identification */
    uint8_t         next_header;               /* upper-layer protocol from first frag */
    uint32_t        reassembled_len;           /* total reassembled datagram length */
    uint16_t        frag_end;                  /* highest byte offset received */
    uint8_t         buf[IPV6_FRAG_BUF_SIZE];   /* reassembly data buffer */
    uint8_t         frag_map[IPV6_FRAG_BUF_SIZE / 8]; /* bitmap: 1 = byte received */
    uint64_t        tick;                      /* timestamp of last activity */
    int             valid;                     /* 1 = slot in use */
    int             first_frag;                /* 1 = first fragment (offset 0) received */
};

static struct ipv6_frag_slot ipv6_frag_slots[IPV6_FRAG_SLOTS];

/* Clear a fragment slot */
static void ipv6_frag_slot_clear(struct ipv6_frag_slot *slot)
{
    if (!slot) return;
    memset(slot, 0, sizeof(*slot));
}

/* IPv6 fragment reassembly statistics — extension of ipv6_frag_stats */
static struct ipv6_frag_stats frag_stats;

/* Evict stale fragment reassembly slots (call from ipv6_frag_poll or
 * before searching for a matching slot) */
static void ipv6_frag_evict_stale(void)
{
    uint64_t now = timer_get_ticks();
    int i;

    for (i = 0; i < IPV6_FRAG_SLOTS; i++) {
        struct ipv6_frag_slot *s = &ipv6_frag_slots[i];
        if (!s->valid)
            continue;
        if (now - s->tick > IPV6_FRAG_TTL_TICKS) {
            kprintf("[ipv6_core] frag timeout: id=%08x\n",
                    s->identification);
            ipv6_frag_slot_clear(s);
            frag_stats.rx_timed_out++;
        }
    }
}

/* Find or allocate a fragment reassembly slot keyed by
 * (src, dst, identification). Returns NULL if all slots exhausted. */
static struct ipv6_frag_slot *ipv6_frag_find(
    const struct in6_addr *src,
    const struct in6_addr *dst,
    uint32_t identification)
{
    int i;

    ipv6_frag_evict_stale();

    /* Return existing matching slot */
    for (i = 0; i < IPV6_FRAG_SLOTS; i++) {
        struct ipv6_frag_slot *s = &ipv6_frag_slots[i];
        if (!s->valid)
            continue;
        if (s->identification == identification &&
            ipv6_addr_equal(&s->src, src) &&
            ipv6_addr_equal(&s->dst, dst))
            return s;
    }

    /* Allocate new empty slot */
    for (i = 0; i < IPV6_FRAG_SLOTS; i++) {
        struct ipv6_frag_slot *s = &ipv6_frag_slots[i];
        if (!s->valid) {
            memset(s, 0, sizeof(*s));
            s->valid = 1;
            memcpy(&s->src, src, sizeof(struct in6_addr));
            memcpy(&s->dst, dst, sizeof(struct in6_addr));
            s->identification = identification;
            s->tick = timer_get_ticks();
            return s;
        }
    }

    /* All slots exhausted */
    frag_stats.rx_oom++;
    kprintf("[ipv6_core] frag: all %d slots exhausted (id=%08x)\n",
            IPV6_FRAG_SLOTS, identification);
    return NULL;
}

/*
 * Handle an incoming IPv6 fragment.
 *
 * Returns:
 *   0 -> packet is not fragmented (no action needed)
 *   1 -> fragment accepted, waiting for more
 *  -1 -> error / fragment discarded
 *
 * On success (returns 1 and all fragments received), reassembles
 * the full datagram and dispatches it via handle_ipv6_packet_reassembled().
 */
static int handle_ipv6_fragment(const struct ipv6_header *ip6,
                                 const struct ipv6_fragment *fh,
                                 const uint8_t *payload,
                                 uint16_t payload_len)
{
    uint16_t frag_off = IPV6_FRAG_OFFSET(fh);
    int      more     = IPV6_FRAG_MORE(fh);

    /* Atomic fragment (offset=0, M=0): pass through as unfragmented */
    if (frag_off == 0 && !more)
        return 0;

    frag_stats.rx_fragments++;

    uint32_t id = ntohl(fh->identification);
    struct ipv6_frag_slot *slot = ipv6_frag_find(&ip6->src_ip,
                                                  &ip6->dst_ip, id);
    if (!slot) {
        frag_stats.rx_dropped++;
        return -1;
    }

    /* Compute the data offset in the reassembled datagram */
    uint32_t data_off = (uint32_t)frag_off * 8;
    uint32_t data_end = data_off + (uint32_t)payload_len;

    /* Validate fragment fits within the reassembly buffer */
    if (data_end > IPV6_FRAG_BUF_SIZE) {
        frag_stats.rx_dropped++;
        kprintf("[ipv6_core] frag overflow: off=%lu part=%u limit=%d (id=%08x)\n",
                (unsigned long)data_off, payload_len,
                IPV6_FRAG_BUF_SIZE, id);
        return -1;
    }

    /* Copy fragment payload into the reassembly buffer */
    memcpy(slot->buf + data_off, payload, payload_len);

    /* Mark bytes received in bitmap */
    {
        uint32_t b;
        for (b = data_off; b < data_end; b++)
            slot->frag_map[b / 8] |= (uint8_t)(1u << (b % 8));
    }

    if (data_end > slot->frag_end)
        slot->frag_end = (uint16_t)data_end;

    if (frag_off == 0) {
        /* First fragment: record the next header for upper-layer dispatch */
        slot->next_header = fh->next_header;
        slot->first_frag = 1;
    }

    slot->tick = timer_get_ticks();

    /* If more fragments are expected, stay in progress */
    if (more)
        return 1;

    /* Last fragment: verify all bytes from 0..frag_end are received */
    {
        uint32_t b;
        for (b = 0; b < slot->frag_end; b++) {
            if (!(slot->frag_map[b / 8] & (uint8_t)(1u << (b % 8))))
                return 1; /* gaps remain, keep waiting */
        }
    }

    /* ---- Reassembly complete ---- */
    slot->valid = 0;
    frag_stats.rx_reassembled++;

    /* Get the next header from the first fragment.  If we never got
     * the first fragment (offset=0), we cannot determine the upper
     * layer protocol — drop the packet. */
    if (!slot->first_frag) {
        frag_stats.rx_dropped++;
        kprintf("[ipv6_core] frag reassembly failed: missing first fragment (id=%08x)\n",
                id);
        return -1;
    }

    /* Reconstruct the IPv6 header for dispatch */
    struct ipv6_header reasm;
    memset(&reasm, 0, sizeof(reasm));
    reasm.vcl_flow       = ip6->vcl_flow;
    reasm.payload_length = htons((uint16_t)slot->frag_end);
    reasm.next_header    = slot->next_header;
    reasm.hop_limit      = ip6->hop_limit;
    memcpy(&reasm.src_ip, &slot->src, sizeof(struct in6_addr));
    memcpy(&reasm.dst_ip, &slot->dst, sizeof(struct in6_addr));

    kprintf("[ipv6_core] frag reassembled: id=%08x size=%u\n",
            id, slot->frag_end);

    /* Dispatch to upper-layer handler */
    handle_ipv6_packet((const uint8_t *)&reasm,
                       (uint16_t)(sizeof(struct ipv6_header) + slot->frag_end));

    return 1;
}

/* Public: poll for expired fragment slots */
void ipv6_frag_poll(void)
{
    ipv6_frag_evict_stale();
}

/* Public: update frag stats from internal counters */
void ipv6_frag_stats_get(struct ipv6_frag_stats *out)
{
    if (!out) return;
    out->rx_fragments   = frag_stats.rx_fragments;
    out->rx_reassembled = frag_stats.rx_reassembled;
    out->rx_timed_out   = frag_stats.rx_timed_out;
    out->rx_dropped     = frag_stats.rx_dropped;
    out->rx_oom         = frag_stats.rx_oom;
}

/*
 * ── IPv6 packet handler (full chain) ───────────────────────────────
 *
 * This is the main IPv6 receive path.  It validates the IPv6 header,
 * walks the extension header chain, and dispatches to the appropriate
 * upper-layer handler.
 *
 * Called from net_poll() via net.c -> handle_ipv6().
 */

void handle_ipv6_packet(const uint8_t *data, uint16_t total_len)
{
    if (!data || total_len < sizeof(struct ipv6_header)) return;

    const struct ipv6_header *ip6 = (const struct ipv6_header *)data;

    /* Verify IPv6 version */
    uint32_t vcl = ntohl(ip6->vcl_flow);
    if ((vcl >> 28) != 6) {
        kprintf("[ipv6_core] not an IPv6 packet (version=%lu)\n",
                (unsigned long)(vcl >> 28));
        net_iface_stats.rx_errors++;
        return;
    }

    /* Walk extension headers */
    uint8_t  proto;
    const uint8_t *payload;
    uint16_t payload_len;
    const struct ipv6_fragment *frag_hdr;

    int ret = ipv6_parse_exthdr(data, total_len,
                                 &proto, &payload, &payload_len,
                                 &frag_hdr);
    if (ret < 0) {
        net_iface_stats.rx_errors++;
        return;
    }

    /* Check for fragmentation */
    if (frag_hdr) {
        int frag_ret = handle_ipv6_fragment(ip6, frag_hdr,
                                             payload, payload_len);
        if (frag_ret != 0) {
            /* Fragment consumed (reassembling, reassembled, or error) */
            net_iface_stats.rx_bytes += total_len;
            return;
        }
        /* Atomic fragment (offset=0, M=0): fall through and dispatch
         * the payload as a normal unfragmented packet.  The upper-layer
         * handlers receive the payload starting after the Fragment Header,
         * which is correct: atomic fragments carry the full upper-layer
         * payload with the Fragment Header prepended. */
        /* payload and payload_len already point past the Fragment Header
         * (set by ipv6_parse_exthdr).  proto contains the final protocol. */
    }

    /* Dispatch to upper-layer handler */
    switch (proto) {
    case IP_PROTO_ICMPV6:
        handle_icmpv6((struct ipv6_header *)data, payload, payload_len);
        break;

    case IP_PROTO_TCP:
        handle_tcp_ipv6((struct ipv6_header *)data, payload, payload_len);
        break;

    case IP_PROTO_UDP:
        handle_udp_ipv6((struct ipv6_header *)data, payload, payload_len);
        break;

    case IPV6_NEXTHDR_NONE:
        /* No next header — silently ignore */
        break;

    default:
        kprintf("[ipv6_core] unknown next header %u (len=%u)\n",
                proto, payload_len);
        break;
    }
}
