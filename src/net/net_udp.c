/* net_udp.c — UDP, DHCP, DNS, HTTP */

#include "net_internal.h"
#include "dns_cache.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "spinlock.h"
#include "socket.h"
#include "sysctl.h"
#include "heap.h"

/* DHCP packet structure */
struct dhcp_packet {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
} __attribute__((packed));

#define DHCP_MAGIC 0x63825363
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

/* DHCP state */
static uint32_t dhcp_xid = 0;
static uint32_t dhcp_offered_ip = 0;
static uint32_t dhcp_server_ip = 0;
static uint32_t dhcp_lease_secs = 0;
static uint64_t dhcp_lease_start = 0;
static volatile int dhcp_state = 0;

static void send_udp_broadcast(uint16_t src_port, uint16_t dst_port,
                                uint32_t src_ip, uint32_t dst_ip,
                                const void *data, uint16_t data_len) {
    uint8_t buf[1500];
    struct ip_header *ip = (struct ip_header *)buf;
    struct udp_header *udp = (struct udp_header *)(buf + sizeof(struct ip_header));

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->src_ip = htonl(src_ip);
    ip->dst_ip = htonl(dst_ip);
    uint16_t udp_len = sizeof(struct udp_header) + data_len;
    ip->total_len = htons(sizeof(struct ip_header) + udp_len);
    ip->id = htons((uint16_t)__sync_fetch_and_add(&net_ip_id_counter, 1));
    ip->checksum = 0;

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(udp_len);
    udp->checksum = 0;

    memcpy(buf + sizeof(struct ip_header) + sizeof(struct udp_header), data, data_len);
    ip->checksum = net_checksum(ip, sizeof(struct ip_header));

    uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    send_eth(bcast_mac, ETH_TYPE_IP, buf, sizeof(struct ip_header) + udp_len);
}

static void dhcp_send_discover(void) {
    uint8_t buf[300];
    memset(buf, 0, sizeof(buf));
    struct dhcp_packet *dhcp = (struct dhcp_packet *)buf;
    dhcp->op = 1;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->xid = htonl(dhcp_xid);
    dhcp->flags = htons(0x8000);
    dhcp->magic_cookie = htonl(DHCP_MAGIC);
    memcpy(dhcp->chaddr, net_our_mac, 6);

    uint8_t *opt = buf + sizeof(struct dhcp_packet);
    *opt++ = 53; *opt++ = 1; *opt++ = DHCP_DISCOVER;
    *opt++ = 255;

    uint16_t pkt_len = (uint16_t)(opt - buf);
    send_udp_broadcast(DHCP_CLIENT_PORT, DHCP_SERVER_PORT, 0, 0xFFFFFFFF, buf, pkt_len);
    dhcp_state = 1;
}

static void dhcp_send_request(void) {
    uint8_t buf[300];
    memset(buf, 0, sizeof(buf));
    struct dhcp_packet *dhcp = (struct dhcp_packet *)buf;
    dhcp->op = 1;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->xid = htonl(dhcp_xid);
    dhcp->flags = htons(0x8000);
    dhcp->magic_cookie = htonl(DHCP_MAGIC);
    memcpy(dhcp->chaddr, net_our_mac, 6);

    uint8_t *opt = buf + sizeof(struct dhcp_packet);
    *opt++ = 53; *opt++ = 1; *opt++ = DHCP_REQUEST;
    *opt++ = 50; *opt++ = 4;
    *opt++ = (uint8_t)((dhcp_offered_ip >> 24) & 0xFF);
    *opt++ = (uint8_t)((dhcp_offered_ip >> 16) & 0xFF);
    *opt++ = (uint8_t)((dhcp_offered_ip >> 8) & 0xFF);
    *opt++ = (uint8_t)(dhcp_offered_ip & 0xFF);
    *opt++ = 54; *opt++ = 4;
    *opt++ = (uint8_t)((dhcp_server_ip >> 24) & 0xFF);
    *opt++ = (uint8_t)((dhcp_server_ip >> 16) & 0xFF);
    *opt++ = (uint8_t)((dhcp_server_ip >> 8) & 0xFF);
    *opt++ = (uint8_t)(dhcp_server_ip & 0xFF);
    *opt++ = 255;

    uint16_t pkt_len = (uint16_t)(opt - buf);
    send_udp_broadcast(DHCP_CLIENT_PORT, DHCP_SERVER_PORT, 0, 0xFFFFFFFF, buf, pkt_len);
    dhcp_state = 2;
}

static void handle_dhcp(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct dhcp_packet)) return;
    const struct dhcp_packet *dhcp = (const struct dhcp_packet *)data;
    if (dhcp->op != 2) return;
    if (ntohl(dhcp->xid) != dhcp_xid) return;
    if (ntohl(dhcp->magic_cookie) != DHCP_MAGIC) return;

    const uint8_t *opt = data + sizeof(struct dhcp_packet);
    const uint8_t *end = data + len;
    uint8_t msg_type = 0;
    uint32_t router = 0;
    uint32_t mask = 0;
    uint32_t dns = 0;
    uint32_t server_id = 0;

    while (opt < end && *opt != 255) {
        if (*opt == 0) { opt++; continue; }
        uint8_t code = *opt++;
        if (opt >= end) break;
        uint8_t olen = *opt++;
        if (opt + olen > end) break;
        if (code == 53 && olen >= 1) msg_type = opt[0];
        if (code == 1 && olen >= 4) mask = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) | ((uint32_t)opt[2] << 8) | opt[3];
        if (code == 3 && olen >= 4) router = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) | ((uint32_t)opt[2] << 8) | opt[3];
        if (code == 6 && olen >= 4) dns = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) | ((uint32_t)opt[2] << 8) | opt[3];
        if (code == 54 && olen >= 4) server_id = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) | ((uint32_t)opt[2] << 8) | opt[3];
        if (code == 51 && olen >= 4)
            dhcp_lease_secs = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) |
                              ((uint32_t)opt[2] << 8) | opt[3];
        opt += olen;
    }

    uint32_t offered = ntohl(dhcp->yiaddr);

    if (msg_type == DHCP_OFFER && dhcp_state == 1) {
        dhcp_offered_ip = offered;
        dhcp_server_ip = server_id;
        if (!dhcp_server_ip) dhcp_server_ip = ntohl(dhcp->siaddr);
        dhcp_send_request();
    } else if (msg_type == DHCP_ACK && dhcp_state == 2) {
        net_our_ip = offered;
        if (mask) net_subnet_mask = mask;
        /* Default to /24 if no mask provided */
        if (!net_subnet_mask)
            net_subnet_mask = (255U << 24) | (255U << 16) | (255U << 8) | 0U;
        if (router) {
            net_gateway_ip = router;
        } else {
            /* Infer gateway as .1 on our subnet */
            net_gateway_ip = (offered & net_subnet_mask) | 1;
        }
        net_dns_server = dns ? dns : net_gateway_ip;
        /* Reset gateway MAC — it may have changed */
        net_gw_mac_known = 0;
        dhcp_state = 3;
        net_dhcp_done = 1;
        dhcp_lease_start = timer_get_ticks();
    }
}

/* DNS state */
static volatile uint32_t dns_result_ip = 0;
static volatile uint32_t dns_result_ttl = 0;
static volatile int dns_reply_received = 0;
static uint16_t dns_txid = 0x1234;

static void handle_dns_reply(const uint8_t *data, uint16_t len) {
    if (len < 12) return;
    uint16_t txid = ((uint16_t)data[0] << 8) | data[1];
    if (txid != dns_txid) return;
    uint16_t flags = ((uint16_t)data[2] << 8) | data[3];
    if (!(flags & 0x8000)) return;
    uint16_t qdcount = ((uint16_t)data[4] << 8) | data[5];
    uint16_t ancount = ((uint16_t)data[6] << 8) | data[7];
    if (ancount == 0) { dns_reply_received = 1; return; }

    int pos = 12;
    for (uint16_t q = 0; q < qdcount && pos < len; q++) {
        int hops = 0;
        while (pos < len && hops < 128) {
            hops++;
            uint8_t lbl = data[pos];
            if (lbl == 0) { pos++; break; }
            if ((lbl & 0xC0) == 0xC0) { pos += 2; break; }
            pos += lbl + 1;
        }
        if (hops >= 128) { dns_reply_received = 1; return; }
        if (pos + 4 > len) { dns_reply_received = 1; return; }
        pos += 4;
    }

    for (uint16_t a = 0; a < ancount && pos + 12 <= len; a++) {
        int hops = 0;
        while (pos < len && hops < 128) {
            hops++;
            if ((data[pos] & 0xC0) == 0xC0) { pos += 2; goto got_name; }
            if (data[pos] == 0) { pos++; break; }
            pos += data[pos] + 1;
        }
        if (hops >= 128) { dns_reply_received = 1; return; }
        got_name:;
        if (pos + 10 > len) break;
        uint16_t rtype = ((uint16_t)data[pos] << 8) | data[pos+1];
        uint16_t rclass __attribute__((unused)) = ((uint16_t)data[pos+2] << 8) | data[pos+3];
        /* Extract TTL from DNS answer (4 bytes at offset 4) */
        uint32_t ttl = ((uint32_t)data[pos+4] << 24) | ((uint32_t)data[pos+5] << 16) |
                       ((uint32_t)data[pos+6] << 8)  | data[pos+7];
        uint16_t rdlen = ((uint16_t)data[pos+8] << 8) | data[pos+9];
        pos += 10;
        if (rtype == 1 && rdlen == 4 && pos + 4 <= len) {
            dns_result_ip = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16) |
                            ((uint32_t)data[pos+2] << 8) | data[pos+3];
            dns_result_ttl = ttl;  /* save TTL for cache store */
            dns_reply_received = 1;
            return;
        }
        pos += rdlen;
    }
    dns_reply_received = 1;
}

/* ICMP Destination Unreachable (type 3, code 3 = Port Unreachable) */
/* ICMP rate limiting: per-destination token bucket to prevent
 * reflection/amplification attacks.
 *
 * sysctl tunables:
 *   icmp_ratelimit  — minimum interval (ms) between ICMP error sends (default 1000)
 *   icmp_ratemask   — bitmask of ICMP types to rate-limit (bit N = type N)
 *                     default bit 12 (ICMP_PARAMPROB)
 */

/* ICMP type constants */
#define ICMP_UNREACH    3   /* Destination Unreachable */
#define ICMP_TIMXCEED   11  /* Time Exceeded */
#define ICMP_PARAMPROB  12  /* Parameter Problem */

/* Per-destination rate limit tracking */
#define ICMP_RATELIMIT_DEST_MAX 16

struct icmp_rate_entry {
    uint32_t dst_ip;
    uint64_t last_send_tick;  /* timer tick of last ICMP error to this dst */
    uint16_t in_use;
};

static struct icmp_rate_entry icmp_rate_table[ICMP_RATELIMIT_DEST_MAX];
static spinlock_t icmp_rate_lock = SPINLOCK_INIT;

/* Sysctl tunables */
static int icmp_ratelimit_ms = 1000;   /* default: 1000ms between ICMP errors */
static uint32_t icmp_ratemask = 0;     /* bitmask, default = ratelimit all ICMP errors */
                                        /* Setting bit N means rate-limit ICMP type N */

/* Sysctl read handler for icmp_ratelimit (milliseconds) */
static int sysctl_read_icmp_ratelimit(char *buf, int max)
{
    char tmp[16];
    int n = 0;
    int v = icmp_ratelimit_ms;
    if (v == 0) { if (n < max - 1) buf[n++] = '0'; }
    else {
        char rev[8];
        int rn = 0;
        while (v > 0) { rev[rn++] = (char)('0' + v % 10); v /= 10; }
        while (rn > 0 && n < max - 1) buf[n++] = rev[--rn];
    }
    if (n < max - 1) buf[n++] = '\n';
    return n;
}

/* Sysctl write handler for icmp_ratelimit */
static int sysctl_write_icmp_ratelimit(const char *buf, int len)
{
    int val = 0;
    int i = 0;
    while (i < len && buf[i] >= '0' && buf[i] <= '9') {
        val = val * 10 + (buf[i] - '0');
        i++;
    }
    if (val < 0 || val > 60000)
        return -EINVAL;
    icmp_ratelimit_ms = val;
    return 0;
}

/* Sysctl read handler for icmp_ratemask (hexadecimal bitmask) */
static int sysctl_read_icmp_ratemask(char *buf, int max)
{
    char tmp[16];
    int n = 0;
    uint32_t v = icmp_ratemask;
    /* Write as 0x%x */
    if (n < max - 1) { buf[n++] = '0'; }
    if (n < max - 1) { buf[n++] = 'x'; }
    int started = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        int digit = (int)((v >> shift) & 0xF);
        if (digit || started || shift == 0) {
            started = 1;
            if (n < max - 1)
                buf[n++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        }
    }
    if (n < max - 1) buf[n++] = '\n';
    return n;
}

/* Sysctl write handler for icmp_ratemask */
static int sysctl_write_icmp_ratemask(const char *buf, int len)
{
    uint32_t val = 0;
    int i = 0;
    /* Skip optional 0x/0X prefix */
    if (i + 1 < len && buf[i] == '0' && (buf[i+1] == 'x' || buf[i+1] == 'X'))
        i += 2;
    while (i < len) {
        char c = buf[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
        else break;
        val = (val << 4) | digit;
        i++;
    }
    icmp_ratemask = val;
    return 0;
}

/* Initialize icmp rate limit sysctls */
void icmp_ratelimit_sysctl_init(void)
{
    sysctl_register("icmp_ratelimit",
                    sysctl_read_icmp_ratelimit,
                    sysctl_write_icmp_ratelimit);
    sysctl_register("icmp_ratemask",
                    sysctl_read_icmp_ratemask,
                    sysctl_write_icmp_ratemask);
}

/* Per-destination ICMP rate limiting check.
 * Returns 1 if the ICMP should be sent, 0 if rate-limited.
 *
 * Uses a token bucket per destination: only one ICMP error per
 * icmp_ratelimit_ms to each destination. */
static int icmp_ratelimit_per_dst(uint32_t dst_ip, uint8_t icmp_type)
{
    /* If ratelimit is 0, no limiting */
    if (icmp_ratelimit_ms == 0)
        return 1;

    /* Check ratemask: if the bit for this ICMP type is NOT set, don't rate-limit it */
    if (icmp_ratemask != 0 && !(icmp_ratemask & (1U << icmp_type)))
        return 1;

    uint64_t now = timer_get_ticks();
    uint64_t min_interval_ticks = (uint64_t)icmp_ratelimit_ms * TIMER_FREQ / 1000;
    if (min_interval_ticks < 1) min_interval_ticks = 1;

    spinlock_acquire(&icmp_rate_lock);

    /* Look for existing entry for this destination */
    int slot = -1;
    int empty_slot = -1;
    for (int i = 0; i < ICMP_RATELIMIT_DEST_MAX; i++) {
        if (icmp_rate_table[i].in_use) {
            if (icmp_rate_table[i].dst_ip == dst_ip) {
                slot = i;
                break;
            }
        } else if (empty_slot < 0) {
            empty_slot = i;
        }
    }

    if (slot >= 0) {
        /* Existing entry — check rate limit */
        uint64_t elapsed = now - icmp_rate_table[slot].last_send_tick;
        if (elapsed < min_interval_ticks) {
            /* Still within rate window — drop */
            spinlock_release(&icmp_rate_lock);
            return 0;
        }
        /* Update timestamp */
        icmp_rate_table[slot].last_send_tick = now;
        spinlock_release(&icmp_rate_lock);
        return 1;
    }

    /* No existing entry — create one if slot available */
    if (empty_slot >= 0) {
        icmp_rate_table[empty_slot].dst_ip = dst_ip;
        icmp_rate_table[empty_slot].last_send_tick = now;
        icmp_rate_table[empty_slot].in_use = 1;
        spinlock_release(&icmp_rate_lock);
        return 1;
    }

    /* No slots available — allow the send (DoS protection would have
     * to wait, but better to send than to silently drop everything) */
    spinlock_release(&icmp_rate_lock);
    return 1;
}

void icmp_send_unreachable(uint32_t dst, uint32_t src, uint8_t *orig_pkt, uint16_t orig_len) {
    (void)src;
    /* Apply per-destination rate limit before generating ICMP error */
    if (!icmp_ratelimit_per_dst(dst, ICMP_UNREACH)) return;

    uint8_t buf[576];  /* ICMP error must fit in 576 bytes guaranteed */
    struct icmp_header *icmp = (struct icmp_header *)buf;
    memset(icmp, 0, sizeof(*icmp));
    icmp->type = ICMP_UNREACH;
    icmp->code = 3;  /* Port Unreachable */

    /* ICMP error payload: IP header of original packet + 8 bytes of original payload */
    uint16_t payload_len = sizeof(struct ip_header) + 8;
    if (payload_len > orig_len) payload_len = orig_len;
    if (sizeof(*icmp) + payload_len > sizeof(buf))
        payload_len = sizeof(buf) - sizeof(*icmp);

    memcpy(buf + sizeof(struct icmp_header), orig_pkt, payload_len);
    uint16_t icmp_len = sizeof(struct icmp_header) + payload_len;
    icmp->checksum = net_checksum(buf, icmp_len);

    send_ip(dst, IP_PROTO_ICMP, buf, icmp_len);
}

/* Send ICMP Time Exceeded (type 11, code 0 = TTL expired) */
void icmp_send_timeexceeded(uint32_t dst, uint32_t src, uint8_t *orig_pkt, uint16_t orig_len)
{
    (void)src;
    /* Apply per-destination rate limit before generating ICMP error */
    if (!icmp_ratelimit_per_dst(dst, ICMP_TIMXCEED)) return;

    uint8_t buf[576];
    struct icmp_header *icmp = (struct icmp_header *)buf;
    memset(icmp, 0, sizeof(*icmp));
    icmp->type = ICMP_TIMXCEED;
    icmp->code = 0;  /* TTL expired in transit */

    /* ICMP error payload: IP header of original packet + 8 bytes of original payload */
    uint16_t payload_len = sizeof(struct ip_header) + 8;
    if (payload_len > orig_len) payload_len = orig_len;
    if (sizeof(*icmp) + payload_len > sizeof(buf))
        payload_len = sizeof(buf) - sizeof(*icmp);

    memcpy(buf + sizeof(struct icmp_header), orig_pkt, payload_len);
    uint16_t icmp_len = sizeof(struct icmp_header) + payload_len;
    icmp->checksum = net_checksum(buf, icmp_len);

    send_ip(dst, IP_PROTO_ICMP, buf, icmp_len);
}

void handle_udp(struct ip_header *ip_hdr, uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct udp_header)) return;
    struct udp_header *udp = (struct udp_header *)payload;
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t udp_len = ntohs(udp->length);
    if (udp_len < sizeof(struct udp_header) || udp_len > len) return;

    /* Verify UDP checksum if present (0 = disabled per RFC 768) */
    if (udp->checksum != 0) {
        uint16_t saved = udp->checksum;
        udp->checksum = 0;
        if (net_transport_checksum(ntohl(ip_hdr->src_ip), ntohl(ip_hdr->dst_ip),
                                   IP_PROTO_UDP, payload, udp_len) != saved)
            return;
    }

    const uint8_t *data = payload + sizeof(struct udp_header);
    uint16_t data_len = (uint16_t)(udp_len - sizeof(struct udp_header));
    uint32_t src_ip = ntohl(ip_hdr->src_ip);

    if (dst_port == DHCP_CLIENT_PORT) {
        handle_dhcp(data, data_len);
        return;
    }
    if (src_port == DNS_PORT && dst_port == 1053) {
        handle_dns_reply(data, data_len);
        return;
    }

    for (int i = 0; i < net_num_udp_bindings; i++) {
        if (net_udp_bindings[i].port == dst_port && net_udp_bindings[i].handler) {
            net_udp_bindings[i].handler(src_ip, src_port, data, data_len);
            return;
        }
    }

    /* No handler found — send ICMP Destination Unreachable (Port Unreachable) */
    if (net_our_ip && dst_port != DHCP_CLIENT_PORT && dst_port != DNS_PORT) {
        icmp_send_unreachable(src_ip, ntohl(ip_hdr->dst_ip),
                              (uint8_t*)ip_hdr, (uint16_t)(sizeof(struct ip_header) + udp_len));
    }
}

void net_udp_bind(uint16_t port, udp_recv_handler handler) {
    if (net_num_udp_bindings >= MAX_UDP_BINDINGS) return;
    net_udp_bindings[net_num_udp_bindings].port = port;
    net_udp_bindings[net_num_udp_bindings].handler = handler;
    net_num_udp_bindings++;
}

/* ── UDP server: userspace listen/recv ─────────────────────────────────────────────── */

#define UDP_LISTEN_MAX   8    /* max simultaneously listened ports */
#define UDP_RING_SIZE    16   /* packets per port ring buffer */
#define UDP_PKT_MAX      1472 /* max UDP payload (Ethernet MTU - headers) */

struct udp_pkt {
    uint8_t  data[UDP_PKT_MAX];
    uint16_t len;
    uint32_t src_ip;
    uint16_t src_port;
};

struct udp_listen_slot {
    uint16_t port;         /* 0 = slot free */
    struct udp_pkt ring[UDP_RING_SIZE];
    volatile int head;     /* producer (ISR/poll) writes here */
    volatile int tail;     /* consumer reads here */
    volatile int count;
};

static struct udp_listen_slot udp_slots[UDP_LISTEN_MAX];

/* Per-slot trampoline: we create one per slot at listen time.
   Since we can't dynamically create functions, we use a small array
   of static handlers that each know their own slot index. */
static void udp_slot_handler_0(uint32_t ip, uint16_t p, const uint8_t *d, uint16_t l);
static void udp_slot_handler_1(uint32_t ip, uint16_t p, const uint8_t *d, uint16_t l);
static void udp_slot_handler_2(uint32_t ip, uint16_t p, const uint8_t *d, uint16_t l);
static void udp_slot_handler_3(uint32_t ip, uint16_t p, const uint8_t *d, uint16_t l);
static void udp_slot_handler_4(uint32_t ip, uint16_t p, const uint8_t *d, uint16_t l);
static void udp_slot_handler_5(uint32_t ip, uint16_t p, const uint8_t *d, uint16_t l);
static void udp_slot_handler_6(uint32_t ip, uint16_t p, const uint8_t *d, uint16_t l);
static void udp_slot_handler_7(uint32_t ip, uint16_t p, const uint8_t *d, uint16_t l);

static void udp_enqueue(int si, uint32_t ip, uint16_t sport,
                        const uint8_t *data, uint16_t len) {
    struct udp_listen_slot *s = &udp_slots[si];
    if (s->count >= UDP_RING_SIZE) return; /* drop */
    struct udp_pkt *pkt = &s->ring[s->head];
    uint16_t copy = len < UDP_PKT_MAX ? len : UDP_PKT_MAX;
    memcpy(pkt->data, data, copy);
    pkt->len = copy;
    pkt->src_ip = ip;
    pkt->src_port = sport;
    s->head = (s->head + 1) % UDP_RING_SIZE;
    s->count++;
}

#define SLOT_HANDLER(N) \
static void udp_slot_handler_##N(uint32_t ip, uint16_t p, const uint8_t *d, uint16_t l) { \
    (void)p; udp_enqueue(N, ip, p, d, l); }
SLOT_HANDLER(0) SLOT_HANDLER(1) SLOT_HANDLER(2) SLOT_HANDLER(3)
SLOT_HANDLER(4) SLOT_HANDLER(5) SLOT_HANDLER(6) SLOT_HANDLER(7)

static const udp_recv_handler slot_handlers[UDP_LISTEN_MAX] = {
    udp_slot_handler_0, udp_slot_handler_1, udp_slot_handler_2, udp_slot_handler_3,
    udp_slot_handler_4, udp_slot_handler_5, udp_slot_handler_6, udp_slot_handler_7
};

int net_udp_listen(uint16_t port) {
    for (int i = 0; i < UDP_LISTEN_MAX; i++) {
        if (udp_slots[i].port == port) return 0; /* already listening */
    }
    for (int i = 0; i < UDP_LISTEN_MAX; i++) {
        if (udp_slots[i].port == 0) {
            udp_slots[i].port  = port;
            udp_slots[i].head  = 0;
            udp_slots[i].tail  = 0;
            udp_slots[i].count = 0;
            net_udp_bind(port, slot_handlers[i]);
            return 0;
        }
    }
    return -1; /* no free slot */
}

int net_udp_recv(uint16_t port, void *buf, uint16_t bufsize,
                 uint32_t *src_ip_out, uint16_t *src_port_out, int timeout_ticks) {
    struct udp_listen_slot *s = NULL;
    for (int i = 0; i < UDP_LISTEN_MAX; i++) {
        if (udp_slots[i].port == port) { s = &udp_slots[i]; break; }
    }
    if (!s) return -1;

    uint64_t start = timer_get_ticks();
    while (s->count == 0) {
        net_poll();
        if (timeout_ticks > 0 && (int)(timer_get_ticks() - start) >= timeout_ticks) return 0;
    }
    if (s->count == 0) return 0;

    struct udp_pkt *pkt = &s->ring[s->tail];
    uint16_t copy = pkt->len < bufsize ? pkt->len : bufsize;
    memcpy(buf, pkt->data, copy);
    if (src_ip_out)   *src_ip_out   = pkt->src_ip;
    if (src_port_out) *src_port_out = pkt->src_port;
    s->tail  = (s->tail + 1) % UDP_RING_SIZE;
    s->count--;
    return (int)copy;
}

void net_udp_unlisten(uint16_t port) {
    for (int i = 0; i < UDP_LISTEN_MAX; i++) {
        if (udp_slots[i].port == port) {
            /* Remove from bindings table */
            for (int j = 0; j < net_num_udp_bindings; j++) {
                if (net_udp_bindings[j].port == port) {
                    /* Shift remaining entries left */
                    for (int k = j; k < net_num_udp_bindings - 1; k++)
                        net_udp_bindings[k] = net_udp_bindings[k + 1];
                    net_num_udp_bindings--;
                    break;
                }
            }
            udp_slots[i].port  = 0;
            udp_slots[i].count = 0;
            return;
        }
    }
}

void net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                  const void *data, uint16_t data_len) {
    uint8_t buf[1500];
    struct udp_header *udp = (struct udp_header *)buf;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    uint16_t udp_len = sizeof(struct udp_header) + data_len;
    udp->length = htons(udp_len);
    udp->checksum = 0;
    memcpy(buf + sizeof(struct udp_header), data, data_len);
    send_ip(dst_ip, IP_PROTO_UDP, buf, udp_len);
}

/*
 * net_udp_send_cached — Fast UDP send path with pre-resolved destination MAC.
 *
 * Builds the complete Ethernet/IP/UDP frame in one shot and hands it to
 * the link layer, bypassing the ARP cache lookup inside send_ip().
 * This is the "connected UDP" fast path — the caller must ensure that
 * @dst_mac is a valid, resolved MAC address (e.g., from a socket's
 * cached_dst_mac populated during connect()).
 *
 * @dst_mac   Pre-resolved Ethernet destination MAC (6 bytes)
 * @dst_ip    Destination IP address (host byte order, for IP header)
 * @src_port  Source UDP port (host byte order)
 * @dst_port  Destination UDP port (host byte order)
 * @data      UDP payload
 * @data_len  Length of payload in bytes
 */
void net_udp_send_cached(const uint8_t *dst_mac, uint32_t dst_ip,
                          uint16_t src_port, uint16_t dst_port,
                          const void *data, uint16_t data_len)
{
    /* ── Build UDP datagram ── */
    uint16_t udp_len = sizeof(struct udp_header) + data_len;
    uint8_t frame[1500];
    memset(frame, 0, sizeof(frame));

    struct udp_header *udp = (struct udp_header *)(frame + sizeof(struct ip_header));
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons(udp_len);
    udp->checksum = 0;  /* UDP checksum is optional (RFC 768) */
    memcpy(frame + sizeof(struct ip_header) + sizeof(struct udp_header), data, data_len);

    /* ── Build IP header ── */
    uint16_t ip_total_len = sizeof(struct ip_header) + udp_len;

    struct ip_header *ip = (struct ip_header *)frame;
    ip->version_ihl = 0x45;
    ip->ttl         = 64;
    ip->protocol    = IP_PROTO_UDP;
    ip->src_ip      = htonl(net_our_ip);
    ip->dst_ip      = htonl(dst_ip);
    ip->total_len   = htons(ip_total_len);
    ip->id          = htons((uint16_t)__sync_fetch_and_add(&net_ip_id_counter, 1));
    ip->checksum    = 0;

    /* Compute IP header checksum */
    ip->checksum = net_checksum(ip, sizeof(struct ip_header));

    /* ── Send via Ethernet directly, bypassing send_ip()'s ARP lookup ── */
    send_eth(dst_mac, ETH_TYPE_IP, frame, ip_total_len);
}

/* ── DNS cache ────────────────────────────────────────────────────── */
/* DNS cache is implemented in dns_cache.c — use the public API. */

uint32_t net_dns_resolve(const char *hostname) {
    int is_ip = 1;
    for (const char *p = hostname; *p; p++) {
        if ((*p < '0' || *p > '9') && *p != '.') { is_ip = 0; break; }
    }
    if (is_ip) {
        uint32_t parts[4] = {0}; int pi = 0;
        for (const char *p = hostname; *p && pi < 4; p++) {
            if (*p >= '0' && *p <= '9') parts[pi] = parts[pi] * 10 + (*p - '0');
            else if (*p == '.') pi++;
        }
        return (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
    }

    /* Check DNS cache first */
    uint32_t cached = dns_cache_lookup(hostname);
    if (cached) return cached;

    uint32_t srv = net_dns_server;
    if (!srv) {
        /* Try reading nameserver from /etc/resolv.conf */
        srv = net_resolv_conf_read_first();
    }
    if (!srv)
        srv = net_gateway_ip;
    if (!srv) return 0;

    uint8_t pkt[512];
    memset(pkt, 0, sizeof(pkt));
    pkt[2] = 0x01; pkt[3] = 0x00;
    pkt[4] = 0; pkt[5] = 1;
    int pos = 12;

    const char *p = hostname;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int lbl_len = (int)(dot - p);
        if (lbl_len > 63) lbl_len = 63;
        pkt[pos++] = (uint8_t)lbl_len;
        for (int i = 0; i < lbl_len; i++) pkt[pos++] = p[i];
        p = *dot ? dot + 1 : dot;
    }
    pkt[pos++] = 0;
    pkt[pos++] = 0; pkt[pos++] = 1;
    pkt[pos++] = 0; pkt[pos++] = 1;

    dns_result_ip = 0;
    dns_result_ttl = 0;
    dns_reply_received = 0;

    for (int attempt = 0; attempt < 3 && !dns_reply_received; attempt++) {
        dns_txid++;
        pkt[0] = (uint8_t)(dns_txid >> 8); pkt[1] = (uint8_t)(dns_txid & 0xFF);
        net_udp_send(srv, 1053, DNS_PORT, pkt, (uint16_t)pos);

        uint64_t start = timer_get_ticks();
        while (!dns_reply_received) {
            net_poll();
            uint64_t now = timer_get_ticks();
            if (now - start > 300) break;  /* 3 second timeout per attempt */
        }
    }
    /* Store result in cache with actual TTL from DNS reply */
    dns_cache_store(hostname, dns_result_ip, dns_result_ttl);
    return dns_result_ip;
}

void net_dhcp_discover(void) {
    dhcp_xid = (uint32_t)(timer_get_ticks() ^ 0xA5A5A5A5u);
    kprintf("[..] DHCP: Sending DISCOVER...\n");
    dhcp_send_discover();

    uint64_t start = timer_get_ticks();
    int resends = 0;
    while (dhcp_state != 3) {
        net_poll();
        uint64_t now = timer_get_ticks();
        if (now - start > 500) break; /* ~5s timeout */
        if (dhcp_state <= 1 && (now - start) > (uint64_t)(resends + 1) * 100 && resends < 4) {
            dhcp_send_discover();
            resends++;
        }
    }

    if (dhcp_state != 3) {
        kprintf("[!!] DHCP: Timeout, using QEMU user-mode defaults\n");
        net_our_ip      = (10U << 24) | (0U << 16) | (2U << 8) | 15U;
        net_gateway_ip  = (10U << 24) | (0U << 16) | (2U << 8) | 2U;
        net_subnet_mask = (255U << 24) | (255U << 16) | (255U << 8) | 0U;
        net_dns_server  = (10U << 24) | (0U << 16) | (2U << 8) | 3U;
    }

    if (dhcp_state == 3)
        kprintf("[OK] DHCP: %u.%u.%u.%u\n",
            (unsigned int)((net_our_ip >> 24) & 0xFF), (unsigned int)((net_our_ip >> 16) & 0xFF),
            (unsigned int)((net_our_ip >> 8) & 0xFF), (unsigned int)(net_our_ip & 0xFF));

    if (net_gateway_ip)
        arp_resolve_gateway();
}

void net_dhcp_renew_if_needed(void) {
    if (dhcp_state != 3 || !dhcp_lease_secs) return;
    uint64_t elapsed = timer_get_ticks() - dhcp_lease_start;
    /* Renew at half lease (ticks ~10ms) */
    if (elapsed < (uint64_t)dhcp_lease_secs * 50) return;
    kprintf("[..] DHCP: Lease 50%% expired, renewing...\n");
    dhcp_state = 1;
    net_dhcp_done = 0;
    /* Send DHCPREQUEST directly (RFC 2131 §4.3.6 — client uses REQUEST to extend lease) */
    dhcp_send_request();
    uint64_t start = timer_get_ticks();
    int resends = 0;
    while (dhcp_state != 3) {
        net_poll();
        uint64_t now = timer_get_ticks();
        if (now - start > 500) break; /* ~5s timeout */
        if (dhcp_state <= 2 && (now - start) > (uint64_t)(resends + 1) * 100 && resends < 4) {
            dhcp_send_request();
            resends++;
        }
    }
    if (dhcp_state == 3) {
        kprintf("[OK] DHCP: Renewed IP %u.%u.%u.%u\n",
            (unsigned int)((net_our_ip >> 24) & 0xFF), (unsigned int)((net_our_ip >> 16) & 0xFF),
            (unsigned int)((net_our_ip >> 8) & 0xFF), (unsigned int)(net_our_ip & 0xFF));
    } else {
        kprintf("[!!] DHCP: Renew failed, will retry\n");
    }
}

/* Parse an absolute URL into host, port, path (modifies all three) */
static void url_parse_absolute(const char *url, char *host, uint16_t *port, char *path) {
    const char *p = url;
    *port = 80;
    if (p[0]=='h' && p[1]=='t' && p[2]=='t' && p[3]=='p') {
        if (p[4]==':' && p[5]=='/' && p[6]=='/') { p += 7; }
        else if (p[4]=='s' && p[5]==':' && p[6]=='/' && p[7]=='/') { *port = 443; p += 8; }
    }
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && *p != ' ' && *p != '\r' && *p != '\n' && hi < 127)
        host[hi++] = *p++;
    host[hi] = '\0';
    if (*p == ':') {
        p++;
        *port = 0;
        while (*p >= '0' && *p <= '9') { *port = (uint16_t)(*port * 10 + (*p - '0')); p++; }
    }
    if (*p == '/') {
        int pi = 0;
        while (*p && *p != ' ' && *p != '\r' && *p != '\n' && pi < 255)
            path[pi++] = *p++;
        path[pi] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }
}

int net_http_get_ex(const char *host_in, uint16_t port_in, const char *path_in,
                    char *buf, int bufsize, int follow_redirects) {
    char host[128], path[256];
    uint16_t port = port_in;
    int i = 0;
    while (host_in[i] && i < 127) { host[i] = host_in[i]; i++; } host[i] = '\0';
    i = 0;
    while (path_in[i] && i < 255) { path[i] = path_in[i]; i++; } path[i] = '\0';

    int ret = -1;
    char *raw = kmalloc(4096);
    if (!raw) return -1;

    int max_redir = follow_redirects ? 5 : 0;

    for (int redir = 0; redir <= max_redir; redir++) {
        uint32_t ip = net_dns_resolve(host);
        if (!ip) { kprintf("DNS resolution failed for %s\n", host); ret = -1; break; }
        kprintf("Resolved %s -> %u.%u.%u.%u\n", host,
            (unsigned int)((ip>>24)&0xFF), (unsigned int)((ip>>16)&0xFF),
            (unsigned int)((ip>>8)&0xFF),  (unsigned int)(ip&0xFF));

        int conn = net_tcp_connect(ip, port);
        if (conn < 0) { kprintf("TCP connect to %u.%u.%u.%u:%u failed\n",
            (unsigned int)((ip>>24)&0xFF), (unsigned int)((ip>>16)&0xFF),
            (unsigned int)((ip>>8)&0xFF),  (unsigned int)(ip&0xFF), (unsigned int)port); ret = -1; break; }

        char req[512];
        int rlen = 0;
        const char *method = "GET ";
        while (*method && rlen < (int)sizeof(req) - 1) req[rlen++] = *method++;
        if (!path[0]) { if (rlen < (int)sizeof(req) - 1) req[rlen++] = '/'; }
        else { const char *pp = path; while (*pp && rlen < (int)sizeof(req) - 1) req[rlen++] = *pp++; }
        const char *ver = " HTTP/1.0\r\nHost: ";
        while (*ver && rlen < (int)sizeof(req) - 1) req[rlen++] = *ver++;
        const char *h = host;
        while (*h && rlen < (int)sizeof(req) - 1) req[rlen++] = *h++;
        const char *end = "\r\nConnection: close\r\n\r\n";
        while (*end && rlen < (int)sizeof(req) - 1) req[rlen++] = *end++;
        if (rlen >= (int)sizeof(req)) { net_tcp_close(conn); ret = -1; break; }
        req[rlen] = '\0';

        net_tcp_send(conn, req, (uint16_t)rlen);

        int total = 0;
        while (total < 4096 - 1) {
            int n = net_tcp_recv(conn, raw + total, (uint16_t)(4096 - 1 - total), 300);
            if (n <= 0) break;
            total += n;
        }
        raw[total] = '\0';
        net_tcp_close(conn);

        /* Parse HTTP status code from "HTTP/1.x NNN ..." */
        int status = 0;
        if (raw[0]=='H' && raw[1]=='T' && raw[2]=='T' && raw[3]=='P') {
            const char *sp = raw;
            while (*sp && *sp != ' ') sp++;
            while (*sp == ' ') sp++;
            while (*sp >= '0' && *sp <= '9') { status = status * 10 + (*sp - '0'); sp++; }
        }

        /* Follow redirect if status is 3xx */
        if (follow_redirects && (status == 301 || status == 302 || status == 303 ||
                                  status == 307 || status == 308)) {
            char *loc = 0;
            for (int j = 0; j < total - 10; j++) {
                if (raw[j]=='L' && raw[j+1]=='o' && raw[j+2]=='c' && raw[j+3]=='a' &&
                    raw[j+4]=='t' && raw[j+5]=='i' && raw[j+6]=='o' && raw[j+7]=='n' &&
                    raw[j+8]==':') {
                    loc = raw + j + 9;
                    while (*loc == ' ') loc++;
                    break;
                }
            }
            if (!loc) { kprintf("Redirect with no Location header\n"); ret = -1; break; }
            char locbuf[256];
            int li = 0;
            while (*loc && *loc != '\r' && *loc != '\n' && li < 255)
                locbuf[li++] = *loc++;
            locbuf[li] = '\0';
            kprintf("Redirect %d -> %s\n", status, locbuf);
            if (locbuf[0] == '/') {
                /* Relative path: keep host and port, update path only */
                for (i = 0; i < 255 && i < li; i++) path[i] = locbuf[i];
                path[i] = '\0';
            } else {
                url_parse_absolute(locbuf, host, &port, path);
            }
            continue;
        }

        /* Extract body (skip headers) */
        char *body = 0;
        for (int j = 0; j < total - 3; j++) {
            if (raw[j]=='\r' && raw[j+1]=='\n' && raw[j+2]=='\r' && raw[j+3]=='\n') {
                body = raw + j + 4;
                break;
            }
        }
        if (body) {
            int bodylen = total - (int)(body - raw);
            if (bodylen > bufsize - 1) bodylen = bufsize - 1;
            memmove(buf, body, bodylen);
            buf[bodylen] = '\0';
            ret = bodylen;
            break;
        }
        if (total > bufsize - 1) total = bufsize - 1;
        memmove(buf, raw, total);
        buf[total] = '\0';
        ret = total;
        break;
    }
    if (ret < 0 && max_redir >= 5)
        kprintf("Too many redirects\n");

    kfree(raw);
    return ret;
}

int net_http_get(const char *host, uint16_t port, const char *path,
                 char *buf, int bufsize) {
    return net_http_get_ex(host, port, path, buf, bufsize, 0);
}

void net_udp_list(void (*cb)(uint16_t port)) {
    for (int i = 0; i < UDP_LISTEN_MAX; i++) {
        if (udp_slots[i].port != 0)
            cb(udp_slots[i].port);
    }
}

/* ── Implement: udp_open ──────────────────────────────── */
int udp_open(void *sk)
{
    if (!sk) return -EINVAL;
    kprintf("[udp] udp_open: UDP socket created\n");
    return 0;
}
/* ── Implement: udp_close ─────────────────────────────── */
int udp_close(void *sk)
{
    if (!sk) return -EINVAL;
    int listener = *(int *)sk;
    if (listener >= 0) {
        net_udp_unlisten((uint16_t)listener);
    }
    return 0;
}
/* ── Implement: udp_connect ───────────────────────────── */
int udp_connect(void *sk, void *addr)
{
    if (!sk || !addr) return -EINVAL;
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    kprintf("[udp] udp_connect: connecting UDP to " NIPQUAD_FMT ":%d\n",
            NIPQUAD(sin->sin_addr.s_addr), ntohs(sin->sin_port));
    return 0;
}
/* ── Implement: udp_sendmsg ───────────────────────────── */
int udp_sendmsg(void *sk, void *msg, size_t len)
{
    if (!sk || !msg) return -EINVAL;
    struct msghdr *hdr = (struct msghdr *)msg;
    if (hdr->msg_iovlen < 1 || !hdr->msg_iov) return -EINVAL;
    const void *data = hdr->msg_iov[0].iov_base;
    uint64_t dlen = hdr->msg_iov[0].iov_len;
    if (dlen > len) dlen = len;
    uint32_t dst_ip = 0;
    uint16_t dst_port = 0;
    if (hdr->msg_name) {
        struct sockaddr_in *sin = (struct sockaddr_in *)hdr->msg_name;
        dst_ip = sin->sin_addr.s_addr;
        dst_port = ntohs(sin->sin_port);
    }
    uint16_t send_len = (uint16_t)(dlen > 1500 ? 1500 : dlen);
    net_udp_send(dst_ip, 0, dst_port, data, send_len);
    return (int)dlen;
}
/* ── Implement: udp_recvmsg ───────────────────────────── */
int udp_recvmsg(void *sk, void *msg, size_t len)
{
    if (!sk || !msg) return -EINVAL;
    struct msghdr *hdr = (struct msghdr *)msg;
    if (hdr->msg_iovlen < 1 || !hdr->msg_iov) return -EINVAL;
    void *buf = hdr->msg_iov[0].iov_base;
    uint64_t bufsize = hdr->msg_iov[0].iov_len;
    if (bufsize > len) bufsize = len;
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t recv_len = (uint16_t)(bufsize > 1500 ? 1500 : bufsize);
    int n = net_udp_recv(0, buf, recv_len, &src_ip, &src_port, 10);
    if (n < 0) return -EAGAIN;
    if (hdr->msg_name) {
        struct sockaddr_in *sin = (struct sockaddr_in *)hdr->msg_name;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(src_port);
        sin->sin_addr.s_addr = src_ip;
        hdr->msg_namelen = sizeof(struct sockaddr_in);
    }
    return n;
}
