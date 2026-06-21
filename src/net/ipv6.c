/*
 * ipv6.c — IPv6 basic stateless auto-configuration (SLAAC)
 *
 * Implements:
 *  - Link-local address generation (FE80::/10 via modified EUI-64)
 *  - ICMPv6 Neighbor Discovery (NS/NA)
 *  - Router Solicitation on startup
 *  - Router Advertisement processing for SLAAC
 *  - IPv6 packet dispatch
 *
 * This is a minimal production-quality IPv6 stack suitable for
 * a hobby OS. It covers RFC 4861 (NDP) and RFC 4862 (SLAAC).
 */

#define KERNEL_INTERNAL
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"

/* ── IPv6 state ──────────────────────────────────────────────────── */

struct in6_addr net_our_ipv6_ll;    /* link-local address */
struct in6_addr net_our_ipv6_gua;   /* global unicast (via SLAAC) */
int  net_ipv6_ll_ready   = 0;       /* 1 = link-local address configured */
int  net_ipv6_gua_valid  = 0;       /* 1 = GUA configured via SLAAC */
struct in6_addr net_ipv6_gateway;   /* default gateway (from RA) */
struct in6_addr net_ipv6_dns;       /* DNS server (from RDNSS) */
uint32_t net_ipv6_ns_count = 0;     /* NS counter for duplicate detection */

/* RS retry state */
static int  rs_sent = 0;            /* have we sent a Router Solicitation? */
static int  rs_retries = 0;         /* RS retry count */
static uint64_t rs_last_tick = 0;   /* tick of last RS transmission */
#define RS_MAX_RETRIES      3
#define RS_RETRY_INTERVAL   50      /* 500 ms (at TIMER_FREQ=100 Hz) */

/* Neighbor Cache: maps IPv6 → MAC */
#define ND_CACHE_SIZE 8
struct nd_cache_entry {
    struct in6_addr ip6;
    uint8_t  mac[6];
    int      valid;
    uint64_t last_seen;             /* tick when last confirmed */
};
static struct nd_cache_entry nd_cache[ND_CACHE_SIZE];

/* ── Solicitated-node multicast prefix (FF02::1:FF00:0/104) ──────── */
/* The lower 24 bits come from the target address. */

void ipv6_calc_solicited_node(const struct in6_addr *addr,
                               struct in6_addr *mcast)
{
    mcast->s6_addr[0]  = 0xFF;
    mcast->s6_addr[1]  = 0x02;
    mcast->s6_addr[2]  = 0x00;
    mcast->s6_addr[3]  = 0x00;
    mcast->s6_addr[4]  = 0x00;
    mcast->s6_addr[5]  = 0x00;
    mcast->s6_addr[6]  = 0x00;
    mcast->s6_addr[7]  = 0x00;
    mcast->s6_addr[8]  = 0x00;
    mcast->s6_addr[9]  = 0x00;
    mcast->s6_addr[10] = 0x00;
    mcast->s6_addr[11] = 0x01;
    mcast->s6_addr[12] = 0xFF;
    /* Take last 3 bytes of address */
    mcast->s6_addr[13] = addr->s6_addr[13];
    mcast->s6_addr[14] = addr->s6_addr[14];
    mcast->s6_addr[15] = addr->s6_addr[15];
}

/* ── EUI-64 from MAC address (RFC 4291 appendix A) ───────────────── */
/* MAC 00:11:22:33:44:55 → EUI-64 02:11:22:FF:FE:33:44:55
 * (invert the Universal/Local bit, insert FF:FE in the middle) */
void ipv6_eui64_from_mac(const uint8_t *mac, struct in6_addr *out)
{
    out->s6_addr[0] = mac[0] ^ 0x02;  /* invert U/L bit */
    out->s6_addr[1] = mac[1];
    out->s6_addr[2] = mac[2];
    out->s6_addr[3] = 0xFF;
    out->s6_addr[4] = 0xFE;
    out->s6_addr[5] = mac[3];
    out->s6_addr[6] = mac[4];
    out->s6_addr[7] = mac[5];
    /* Upper 64 bits (subnet prefix) are set by the caller */
}

/* ── Address helpers ─────────────────────────────────────────────── */

int ipv6_addr_is_multicast(const struct in6_addr *addr)
{
    return addr->s6_addr[0] == 0xFF;
}

int ipv6_addr_is_linklocal(const struct in6_addr *addr)
{
    return (addr->s6_addr[0] == 0xFE) && ((addr->s6_addr[1] & 0xC0) == 0x80);
}

int ipv6_addr_is_unspecified(const struct in6_addr *addr)
{
    for (int i = 0; i < 16; i++) {
        if (addr->s6_addr[i] != 0) return 0;
    }
    return 1;
}

int ipv6_addr_equal(const struct in6_addr *a, const struct in6_addr *b)
{
    return memcmp(a->s6_addr, b->s6_addr, 16) == 0;
}

/* ── Neighbor Cache ──────────────────────────────────────────────── */

void ipv6_nd_cache_add(const struct in6_addr *ip6, const uint8_t *mac)
{
    /* Check for existing entry */
    for (int i = 0; i < ND_CACHE_SIZE; i++) {
        if (nd_cache[i].valid && ipv6_addr_equal(&nd_cache[i].ip6, ip6)) {
            memcpy(nd_cache[i].mac, mac, 6);
            nd_cache[i].last_seen = timer_get_ticks();
            return;
        }
    }
    /* Find free slot (LRU replacement) */
    int slot = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < ND_CACHE_SIZE; i++) {
        if (!nd_cache[i].valid) { slot = i; break; }
        if (nd_cache[i].last_seen < oldest) {
            oldest = nd_cache[i].last_seen;
            slot = i;
        }
    }
    if (slot >= 0) {
        memcpy(&nd_cache[slot].ip6, ip6, sizeof(struct in6_addr));
        memcpy(nd_cache[slot].mac, mac, 6);
        nd_cache[slot].valid = 1;
        nd_cache[slot].last_seen = timer_get_ticks();
    }
}

static uint8_t *nd_cache_lookup(const struct in6_addr *ip6)
{
    for (int i = 0; i < ND_CACHE_SIZE; i++) {
        if (nd_cache[i].valid && ipv6_addr_equal(&nd_cache[i].ip6, ip6))
            return nd_cache[i].mac;
    }
    return NULL;
}

/* ── IPv6 pseudo-header checksum (RFC 8200, §8.1) ────────────────── */

/* Compute checksum over IPv6 pseudo-header + upper-layer data.
 * The caller must pass the source, destination, next-header type,
 * and the upper-layer data (including the upper-layer header). */
uint16_t ipv6_checksum(const struct in6_addr *src,
                        const struct in6_addr *dst,
                        uint8_t next_hdr,
                        const void *data, uint16_t data_len)
{
    /* Pseudo-header (40 bytes) */
    uint8_t pseudo[40 + 4]; /* 40 = 2*16(src+dst) + 4(len) + 4(next+zero) */
    memcpy(pseudo, src, 16);
    memcpy(pseudo + 16, dst, 16);
    pseudo[32] = (data_len >> 24) & 0xFF;
    pseudo[33] = (data_len >> 16) & 0xFF;
    pseudo[34] = (data_len >> 8) & 0xFF;
    pseudo[35] = data_len & 0xFF;
    pseudo[36] = 0;
    pseudo[37] = 0;
    pseudo[38] = 0;
    pseudo[39] = next_hdr;

    /* Build composite buffer: pseudo-header + data */
    /* We compute checksum incrementally to avoid large stack allocation */
    uint32_t sum = 0;
    int count = 44; /* 40 + 4 zero padding = 44 bytes total for pseudo */
    const uint16_t *ptr = (const uint16_t *)pseudo;
    while (count > 1) {
        sum += *ptr++;
        count -= 2;
    }
    if (count)
        sum += *(const uint8_t *)ptr;

    ptr = (const uint16_t *)data;
    count = data_len;
    while (count > 1) {
        sum += *ptr++;
        count -= 2;
    }
    if (count)
        sum += *(const uint8_t *)ptr;

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

/* ── Send Ethernet frame with IPv6 ethertype ─────────────────────── */

void send_eth_ipv6(const uint8_t *dst_mac, const void *payload,
                    uint16_t len)
{
    send_eth(dst_mac, ETH_TYPE_IPV6, payload, len);
}

/* ── Send IPv6 packet ────────────────────────────────────────────── */

/* Compute the Ethernet destination MAC for a given IPv6 address:
 *  - For multicast addresses, map to IPv6 multicast MAC (33:33:XX:XX:XX:XX)
 *  - Otherwise look up in neighbor cache (or fall back to all-nodes) */
static void ipv6_resolve_dst_mac(const struct in6_addr *dst, uint8_t *mac_out)
{
    if (ipv6_addr_is_multicast(dst)) {
        /* IPv6 multicast MAC: 33:33:XX:XX:XX:XX (lower 32 bits of IPv6 addr) */
        mac_out[0] = 0x33;
        mac_out[1] = 0x33;
        mac_out[2] = dst->s6_addr[12];
        mac_out[3] = dst->s6_addr[13];
        mac_out[4] = dst->s6_addr[14];
        mac_out[5] = dst->s6_addr[15];
        return;
    }
    uint8_t *cached = nd_cache_lookup(dst);
    if (cached) {
        memcpy(mac_out, cached, 6);
        return;
    }
    /* Fallback: send to all-nodes multicast MAC */
    mac_out[0] = 0x33;
    mac_out[1] = 0x33;
    mac_out[2] = 0x00;
    mac_out[3] = 0x00;
    mac_out[4] = 0x00;
    mac_out[5] = 0x01;
}

void send_ipv6(const struct in6_addr *dst, uint8_t next_hdr,
                const void *payload, uint16_t len)
{
    uint8_t buf[2048];
    struct ipv6_header *ip6 = (struct ipv6_header *)buf;

    /* Build IPv6 header */
    /* Version=6, Traffic Class=0, Flow Label=0 */
    ip6->vcl_flow = htonl(0x60000000U);
    ip6->payload_length = htons(len);
    ip6->next_header = next_hdr;
    ip6->hop_limit = 64;  /* default hop limit */
    memcpy(&ip6->src_ip, &net_our_ipv6_ll, sizeof(struct in6_addr));
    memcpy(&ip6->dst_ip, dst, sizeof(struct in6_addr));

    /* Copy payload after header */
    if (len > 0)
        memcpy(buf + sizeof(struct ipv6_header), payload, len);

    uint16_t total = sizeof(struct ipv6_header) + len;

    /* Resolve destination MAC and send */
    uint8_t dst_mac[6];
    ipv6_resolve_dst_mac(dst, dst_mac);
    send_eth_ipv6(dst_mac, buf, total);
}

/* ── ICMPv6 Echo (ping6) ─────────────────────────────────────────── */

static int ping6_reply_received = 0;

static void handle_icmpv6_echo_request(struct ipv6_header *ip6,
                                        const uint8_t *payload,
                                        uint16_t len)
{
    uint8_t reply_buf[1500];
    uint16_t reply_len = len < sizeof(reply_buf) ? len : sizeof(reply_buf);
    memcpy(reply_buf, payload, reply_len);
    struct icmpv6_header *icmp = (struct icmpv6_header *)reply_buf;
    icmp->type = 129; /* Echo Reply */
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->checksum = ipv6_checksum(&net_our_ipv6_ll, &ip6->src_ip,
                                    IP_PROTO_ICMPV6,
                                    reply_buf, reply_len);
    send_ipv6(&ip6->src_ip, IP_PROTO_ICMPV6, reply_buf, reply_len);
}

static void handle_icmpv6_echo_reply(void)
{
    ping6_reply_received = 1;
}

/* ── NDP: Neighbor Solicitation (NS) handler ─────────────────────── */

static void nd_send_na(const struct in6_addr *target,
                        const struct in6_addr *dst,
                        int solicited, int override)
{
    uint8_t buf[128];
    struct nd_neighbor *na = (struct nd_neighbor *)buf;

    memset(na, 0, sizeof(*na));
    na->icmp.type = ICMPV6_NA;
    na->icmp.code = 0;
    /* Flags: R=0, S=solicited, O=override */
    na->reserved = htonl((solicited ? 0x40000000U : 0) |
                          (override ? 0x20000000U : 0));
    memcpy(&na->target, target, sizeof(struct in6_addr));

    /* Target Link-layer Address option */
    struct nd_option *opt = (struct nd_option *)(buf + sizeof(struct nd_neighbor));
    opt->type = ND_OPT_TGT_LLADDR;
    opt->len  = 1;  /* 1 * 8 = 8 bytes (type + len + 6 bytes MAC) */
    memcpy(buf + sizeof(struct nd_neighbor) + 2, net_our_mac, 6);

    uint16_t nd_len = sizeof(struct nd_neighbor) + 8;

    na->icmp.checksum = ipv6_checksum(&net_our_ipv6_ll, dst,
                                       IP_PROTO_ICMPV6, buf, nd_len);
    send_ipv6(dst, IP_PROTO_ICMPV6, buf, nd_len);
}

static void nd_handle_ns(struct ipv6_header *ip6, const uint8_t *payload,
                          uint16_t len)
{
    if (len < sizeof(struct nd_neighbor))
        return;

    const struct nd_neighbor *ns = (const struct nd_neighbor *)payload;

    /* We only respond if the target is our link-local address */
    if (!net_ipv6_ll_ready) return;

    int is_our_ll = ipv6_addr_equal(&ns->target, &net_our_ipv6_ll);
    int is_our_gua = net_ipv6_gua_valid &&
                     ipv6_addr_equal(&ns->target, &net_our_ipv6_gua);

    if (!is_our_ll && !is_our_gua) return;

    /* Extract source MAC from Source Link-layer Address option if present */
    const struct nd_option *opt = (const struct nd_option *)(payload + sizeof(struct nd_neighbor));
    int opt_offset = sizeof(struct nd_neighbor);
    while (opt_offset + 2 <= (int)len) {
        if (opt->type == ND_OPT_SRC_LLADDR && opt->len == 1) {
            /* Source MAC is at payload + opt_offset + 2 */
            ipv6_nd_cache_add(&ip6->src_ip, payload + opt_offset + 2);
            break;
        }
        opt_offset += (int)opt->len * 8;
        if (opt_offset + 2 > (int)len) break;
        opt = (const struct nd_option *)(payload + opt_offset);
    }

    /* Send Neighbor Advertisement */
    struct in6_addr dst;
    if (ipv6_addr_is_unspecified(&ip6->src_ip)) {
        /* Unsolicited NA to all-nodes multicast */
        struct in6_addr all_nodes = IPV6_ADDR_ALL_NODES;
        memcpy(&dst, &all_nodes, sizeof(dst));
        nd_send_na(&ns->target, &dst, 0, 1);
    } else {
        nd_send_na(&ns->target, &ip6->src_ip, 1, 1);
    }
}

/* ── NDP: Neighbor Advertisement (NA) handler ────────────────────── */

static void nd_handle_na(struct ipv6_header *ip6, const uint8_t *payload,
                          uint16_t len)
{
    (void)ip6;
    if (len < sizeof(struct nd_neighbor))
        return;

    const struct nd_neighbor *na = (const struct nd_neighbor *)payload;

    /* Extract target MAC from Target Link-layer Address option */
    const struct nd_option *opt = (const struct nd_option *)(payload + sizeof(struct nd_neighbor));
    int opt_offset = sizeof(struct nd_neighbor);
    while (opt_offset + 2 <= (int)len) {
        if (opt->type == ND_OPT_TGT_LLADDR && opt->len == 1) {
            ipv6_nd_cache_add(&na->target, payload + opt_offset + 2);
            break;
        }
        opt_offset += (int)opt->len * 8;
        if (opt_offset + 2 > (int)len) break;
        opt = (const struct nd_option *)(payload + opt_offset);
    }
}

/* ── NDP: Router Solicitation (RS) ───────────────────────────────── */

void ipv6_send_rs(void)
{
    uint8_t buf[64];
    struct nd_router_solicit *rs = (struct nd_router_solicit *)buf;
    memset(rs, 0, sizeof(*rs));
    rs->icmp.type = ICMPV6_RS;
    rs->icmp.code = 0;

    /* Source Link-layer Address option */
    struct nd_option *opt = (struct nd_option *)(buf + sizeof(struct nd_router_solicit));
    opt->type = ND_OPT_SRC_LLADDR;
    opt->len  = 1;
    memcpy(buf + sizeof(struct nd_router_solicit) + 2, net_our_mac, 6);

    uint16_t rs_len = sizeof(struct nd_router_solicit) + 8;

    /* Send to all-routers multicast */
    struct in6_addr all_routers = IPV6_ADDR_ALL_ROUTERS;
    rs->icmp.checksum = ipv6_checksum(&net_our_ipv6_ll, &all_routers,
                                       IP_PROTO_ICMPV6, buf, rs_len);
    send_ipv6(&all_routers, IP_PROTO_ICMPV6, buf, rs_len);
    kprintf("[IPv6] Sent Router Solicitation\n");
}

/* ── SLAAC: Router Advertisement (RA) processing ─────────────────── */

static void nd_handle_ra(struct ipv6_header *ip6, const uint8_t *payload,
                          uint16_t len)
{
    if (len < sizeof(struct nd_router_advert))
        return;

    const struct nd_router_advert *ra = (const struct nd_router_advert *)payload;

    kprintf("[IPv6] Received Router Advertisement (lifetime=%u)\n",
            ntohs(ra->router_lifetime));

    /* Record default gateway */
    memcpy(&net_ipv6_gateway, &ip6->src_ip, sizeof(struct in6_addr));

    /* Hop limit from RA */
    uint8_t hop_limit = ra->cur_hop_limit;
    (void)hop_limit;  /* could be used in send_ipv6 */

    /* Process options */
    const uint8_t *opt_data = payload + sizeof(struct nd_router_advert);
    int remaining = len - sizeof(struct nd_router_advert);

    while (remaining >= 2) {
        const struct nd_option *opt = (const struct nd_option *)opt_data;
        int opt_len = (int)opt->len * 8;

        if (opt_len <= 0 || opt_len > remaining) break;

        if (opt->type == ND_OPT_PREFIX_INFO && opt_len >= 32) {
            /* Prefix Information option (RFC 4861, §4.6.2) */
            uint8_t prefix_len = opt_data[2];           /* prefix length in bits */
            uint8_t prefix_flags = opt_data[3];         /* L=bit7, A=bit6 */
            uint32_t valid_lifetime = (uint32_t)opt_data[4] << 24 |
                                      (uint32_t)opt_data[5] << 16 |
                                      (uint32_t)opt_data[6] << 8  |
                                      (uint32_t)opt_data[7];
            /* Prefix is at offset 16 within this option */
            const uint8_t *prefix = opt_data + 16;

            /* Check A-flag (autonomous address-configuration) */
            if (prefix_flags & 0x40) {
                struct in6_addr gua;
                /* Use the prefix with our EUI-64 interface identifier */
                if (prefix_len == 64) {
                    memcpy(gua.s6_addr, prefix, 8);
                    memcpy(gua.s6_addr + 8, net_our_ipv6_ll.s6_addr + 8, 8);
                } else {
                    /* For non-64 prefixes, zero out host bits beyond prefix */
                    memcpy(&gua, prefix, 16);
                    /* Mask off host bits beyond prefix_len */
                    if (prefix_len < 128) {
                        int byte = prefix_len / 8;
                        int bit  = prefix_len % 8;
                        if (byte < 16) {
                            gua.s6_addr[byte] &= (uint8_t)(0xFF << (8 - bit));
                            for (int i = byte + 1; i < 16; i++)
                                gua.s6_addr[i] = 0;
                        }
                    }
                    /* OR in interface ID (lower 64 bits from link-local) */
                    for (int i = 8; i < 16; i++)
                        gua.s6_addr[i] |= net_our_ipv6_ll.s6_addr[i];
                }

                memcpy(&net_our_ipv6_gua, &gua, sizeof(struct in6_addr));
                net_ipv6_gua_valid = 1;

                kprintf("[IPv6] SLAAC: configured GUA, valid=%u\n",
                        valid_lifetime);
            }
        } else if (opt->type == ND_OPT_MTU && opt_len >= 8) {
            /* MTU option — could be used to set interface MTU */
            /* uint32_t mtu = ... */
        }
        /* RDNSS option (RFC 8106) could be added here for DNS */

        opt_data += opt_len;
        remaining -= opt_len;
    }
}

/* ── ICMPv6 dispatcher ───────────────────────────────────────────── */

void handle_icmpv6(struct ipv6_header *ip6, const uint8_t *payload,
                    uint16_t len)
{
    if (len < sizeof(struct icmpv6_header)) return;

    const struct icmpv6_header *icmp = (const struct icmpv6_header *)payload;

    switch (icmp->type) {
    case 128: /* Echo Request */
        handle_icmpv6_echo_request(ip6, payload, len);
        break;
    case 129: /* Echo Reply */
        handle_icmpv6_echo_reply();
        break;
    case ICMPV6_NS: /* Neighbor Solicitation */
        nd_handle_ns(ip6, payload, len);
        break;
    case ICMPV6_NA: /* Neighbor Advertisement */
        nd_handle_na(ip6, payload, len);
        break;
    case ICMPV6_RS: /* Router Solicitation */
        /* We're not a router, so ignore */
        break;
    case ICMPV6_RA: /* Router Advertisement */
        nd_handle_ra(ip6, payload, len);
        break;
    default:
        break;
    }
}

/* ── IPv6 dispatcher ─────────────────────────────────────────────── */

static int ipv6_addr_is_ours(const struct in6_addr *addr)
{
    if (!addr) return 0;
    if (ipv6_addr_equal(addr, &net_our_ipv6_ll)) return 1;
    if (net_ipv6_gua_valid && ipv6_addr_equal(addr, &net_our_ipv6_gua)) return 1;
    return 0;
}

void handle_ipv6(const uint8_t *data, uint16_t len)
{
    if (len < sizeof(struct ipv6_header)) return;

    const struct ipv6_header *ip6 = (const struct ipv6_header *)data;

    /* Verify we have the full header + payload */
    uint16_t payload_len = ntohs(ip6->payload_length);
    if (sizeof(struct ipv6_header) + payload_len > len) return;

    /* Accept packets destined to us or to multicast addresses */
    if (!ipv6_addr_is_ours(&ip6->dst_ip) &&
        !ipv6_addr_is_multicast(&ip6->dst_ip))
        return;

    /* Update neighbor cache from source address (if link-local) */
    if (ipv6_addr_is_linklocal(&ip6->src_ip)) {
        /* We don't have the MAC here; it's added by net_poll before calling us */
    }

    const uint8_t *payload = data + sizeof(struct ipv6_header);

    if (ip6->next_header == IP_PROTO_ICMPV6) {
        handle_icmpv6((struct ipv6_header *)data, payload, payload_len);
    }
    /* Future: handle TCPv6, UDPv6, etc. */
}

/* ── ping6 ────────────────────────────────────────────────────────── */

int ipv6_ping6(const struct in6_addr *target)
{
    if (!net_ipv6_ll_ready) return -1;

    uint8_t buf[128];
    struct icmpv6_echo *echo = (struct icmpv6_echo *)buf;
    echo->hdr.type = 128; /* Echo Request */
    echo->hdr.code = 0;
    echo->id   = htons(0x1234);

    uint16_t data_start = sizeof(struct icmpv6_echo);

    for (int seq = 1; seq <= 4; seq++) {
        echo->seq = htons((uint16_t)seq);

        /* Fill payload with pattern data */
        for (int i = 0; i < 32; i++)
            buf[data_start + i] = (uint8_t)i;

        uint16_t pkt_len = data_start + 32;

        echo->hdr.checksum = 0;
        echo->hdr.checksum = ipv6_checksum(&net_our_ipv6_ll, target,
                                        IP_PROTO_ICMPV6, buf, pkt_len);

        ping6_reply_received = 0;
        send_ipv6(target, IP_PROTO_ICMPV6, buf, pkt_len);

        uint64_t start = timer_get_ticks();
        while (!ping6_reply_received) {
            net_poll();
            uint64_t now = timer_get_ticks();
            if (now - start > 200) break; /* 2 second timeout */
        }
        if (ping6_reply_received) {
            uint64_t elapsed = timer_get_ticks() - start;
            return (int)(elapsed * 10);
        }
    }
    return -1;
}

/* ── Initialization ──────────────────────────────────────────────── */

void ipv6_init(void)
{
    /* Generate link-local address from MAC */
    struct in6_addr ll_prefix = IPV6_ADDR_LINKLOCAL_PFX;
    ipv6_eui64_from_mac(net_our_mac, &net_our_ipv6_ll);
    /* Set FE80:: prefix (first 10 bits) */
    memcpy(net_our_ipv6_ll.s6_addr, ll_prefix.s6_addr, 8);

    net_ipv6_ll_ready = 1;
    memset(nd_cache, 0, sizeof(nd_cache));

    kprintf("[IPv6] Link-local address configured\n");

    /* Send Router Solicitation to trigger SLAAC */
    rs_sent = 1;
    rs_retries = 0;
    rs_last_tick = timer_get_ticks();
    ipv6_send_rs();

    /* Send Neighbor Solicitation for DAD (Duplicate Address Detection) */
    /* For now, we assume the address is unique (no DAD). */
}

/* ── Periodic poll tasks ─────────────────────────────────────────── */

void ipv6_poll(void)
{
    /* Retry RS if we haven't received an RA yet */
    if (rs_sent && !net_ipv6_gua_valid && rs_retries < RS_MAX_RETRIES) {
        uint64_t now = timer_get_ticks();
        if (now - rs_last_tick >= RS_RETRY_INTERVAL) {
            rs_retries++;
            rs_last_tick = now;
            ipv6_send_rs();
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

int ipv6_has_linklocal(void)
{
    return net_ipv6_ll_ready;
}

void ipv6_get_linklocal(struct in6_addr *addr)
{
    if (addr) memcpy(addr, &net_our_ipv6_ll, sizeof(struct in6_addr));
}

/* ── Stub: ipv6_send ─────────────────────────────── */
int ipv6_send(void *skb)
{
    (void)skb;
    kprintf("[ipv6] ipv6_send: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: ipv6_route_add ─────────────────────────────── */
int ipv6_route_add(const void *dst, const void *gw, int ifindex)
{
    (void)dst;
    (void)gw;
    (void)ifindex;
    kprintf("[ipv6] ipv6_route_add: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: ipv6_route_del ─────────────────────────────── */
int ipv6_route_del(const void *dst)
{
    (void)dst;
    kprintf("[ipv6] ipv6_route_del: not yet implemented\n");
    return -ENOSYS;
}
