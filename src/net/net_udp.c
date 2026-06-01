/* net_udp.c — UDP, DHCP, DNS, HTTP */

#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"

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
    ip->id = htons(net_ip_id_counter++);
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

    uint16_t pkt_len = opt - buf;
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
    *opt++ = (dhcp_offered_ip >> 24) & 0xFF;
    *opt++ = (dhcp_offered_ip >> 16) & 0xFF;
    *opt++ = (dhcp_offered_ip >> 8) & 0xFF;
    *opt++ = dhcp_offered_ip & 0xFF;
    *opt++ = 54; *opt++ = 4;
    *opt++ = (dhcp_server_ip >> 24) & 0xFF;
    *opt++ = (dhcp_server_ip >> 16) & 0xFF;
    *opt++ = (dhcp_server_ip >> 8) & 0xFF;
    *opt++ = dhcp_server_ip & 0xFF;
    *opt++ = 255;

    uint16_t pkt_len = opt - buf;
    send_udp_broadcast(DHCP_CLIENT_PORT, DHCP_SERVER_PORT, 0, 0xFFFFFFFF, buf, pkt_len);
    dhcp_state = 2;
}

static void handle_dhcp(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct dhcp_packet)) return;
    struct dhcp_packet *dhcp = (struct dhcp_packet *)data;
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
        uint16_t rdlen = ((uint16_t)data[pos+8] << 8) | data[pos+9];
        pos += 10;
        if (rtype == 1 && rdlen == 4 && pos + 4 <= len) {
            dns_result_ip = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16) |
                            ((uint32_t)data[pos+2] << 8) | data[pos+3];
            dns_reply_received = 1;
            return;
        }
        pos += rdlen;
    }
    dns_reply_received = 1;
}

/* ICMP Destination Unreachable (type 3, code 3 = Port Unreachable) */
void icmp_send_unreachable(uint32_t dst, uint32_t src, uint8_t *orig_pkt, uint16_t orig_len) {
    uint8_t buf[576];  /* ICMP error must fit in 576 bytes guaranteed */
    struct icmp_header *icmp = (struct icmp_header *)buf;
    memset(icmp, 0, sizeof(*icmp));
    icmp->type = 3;
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

void handle_udp(struct ip_header *ip_hdr, const uint8_t *payload, uint16_t len) {
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
    uint16_t data_len = udp_len - sizeof(struct udp_header);
    uint32_t src_ip = ntohl(ip_hdr->src_ip);

    if (dst_port == DHCP_CLIENT_PORT) {
        handle_dhcp(data, data_len);
        return;
    }
    if (src_port == DNS_PORT) {
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

static udp_recv_handler slot_handlers[UDP_LISTEN_MAX] = {
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

/* ── DNS cache ────────────────────────────────────────────────────── */
#define DNS_CACHE_SIZE  16
#define DNS_CACHE_TTL   3000   /* ticks (~30 seconds at 100 Hz) */

static struct {
    char     name[64];
    uint32_t ip;
    uint64_t expires;   /* timer_get_ticks() + DNS_CACHE_TTL */
    int      valid;
} dns_cache[DNS_CACHE_SIZE];

static uint32_t dns_cache_lookup(const char *name) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && strcmp(dns_cache[i].name, name) == 0) {
            if (now < dns_cache[i].expires) return dns_cache[i].ip;
            dns_cache[i].valid = 0;
        }
    }
    return 0;
}

static void dns_cache_store(const char *name, uint32_t ip) {
    if (!ip) return;
    /* First, look for an existing entry with the same name */
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && strcmp(dns_cache[i].name, name) == 0) {
            dns_cache[i].ip      = ip;
            dns_cache[i].expires = timer_get_ticks() + DNS_CACHE_TTL;
            return;
        }
    }
    /* Find LRU or invalid slot */
    int slot = 0;
    uint64_t oldest = dns_cache[0].expires;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid) { slot = i; break; }
        if (dns_cache[i].expires < oldest) { oldest = dns_cache[i].expires; slot = i; }
    }
    strncpy(dns_cache[slot].name, name, 63);
    dns_cache[slot].name[63] = '\0';
    dns_cache[slot].ip      = ip;
    dns_cache[slot].expires = timer_get_ticks() + DNS_CACHE_TTL;
    dns_cache[slot].valid   = 1;
}

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

    uint32_t srv = net_dns_server ? net_dns_server : net_gateway_ip;
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
        int lbl_len = dot - p;
        if (lbl_len > 63) lbl_len = 63;
        pkt[pos++] = (uint8_t)lbl_len;
        for (int i = 0; i < lbl_len; i++) pkt[pos++] = p[i];
        p = *dot ? dot + 1 : dot;
    }
    pkt[pos++] = 0;
    pkt[pos++] = 0; pkt[pos++] = 1;
    pkt[pos++] = 0; pkt[pos++] = 1;

    dns_result_ip = 0;
    dns_reply_received = 0;

    for (int attempt = 0; attempt < 3 && !dns_reply_received; attempt++) {
        dns_txid++;
        pkt[0] = dns_txid >> 8; pkt[1] = dns_txid & 0xFF;
        net_udp_send(srv, 1053, DNS_PORT, pkt, pos);

        uint64_t start = timer_get_ticks();
        while (!dns_reply_received) {
            net_poll();
            uint64_t now = timer_get_ticks();
            if (now - start > 300) break;  /* 3 second timeout per attempt */
        }
    }
    /* Store result in cache */
    dns_cache_store(hostname, dns_result_ip);
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
            (uint64_t)((net_our_ip >> 24) & 0xFF), (uint64_t)((net_our_ip >> 16) & 0xFF),
            (uint64_t)((net_our_ip >> 8) & 0xFF), (uint64_t)(net_our_ip & 0xFF));

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
            (uint64_t)((net_our_ip >> 24) & 0xFF), (uint64_t)((net_our_ip >> 16) & 0xFF),
            (uint64_t)((net_our_ip >> 8) & 0xFF), (uint64_t)(net_our_ip & 0xFF));
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
        while (*p >= '0' && *p <= '9') { *port = *port * 10 + (*p - '0'); p++; }
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

    static char raw[4096];
    int max_redir = follow_redirects ? 5 : 0;

    for (int redir = 0; redir <= max_redir; redir++) {
        uint32_t ip = net_dns_resolve(host);
        if (!ip) { kprintf("DNS resolution failed for %s\n", host); return -1; }
        kprintf("Resolved %s -> %u.%u.%u.%u\n", host,
            (uint64_t)((ip>>24)&0xFF), (uint64_t)((ip>>16)&0xFF),
            (uint64_t)((ip>>8)&0xFF),  (uint64_t)(ip&0xFF));

        int conn = net_tcp_connect(ip, port);
        if (conn < 0) { kprintf("TCP connect to %u.%u.%u.%u:%u failed\n",
            (uint64_t)((ip>>24)&0xFF), (uint64_t)((ip>>16)&0xFF),
            (uint64_t)((ip>>8)&0xFF),  (uint64_t)(ip&0xFF), (uint64_t)port); return -1; }

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
        if (rlen >= (int)sizeof(req)) { net_tcp_close(conn); return -1; }
        req[rlen] = '\0';

        net_tcp_send(conn, req, rlen);

        int total = 0;
        while (total < (int)sizeof(raw) - 1) {
            int n = net_tcp_recv(conn, raw + total, sizeof(raw) - 1 - total, 300);
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
            if (!loc) { kprintf("Redirect with no Location header\n"); return -1; }
            char locbuf[256];
            int li = 0;
            while (*loc && *loc != '\r' && *loc != '\n' && li < 255)
                locbuf[li++] = *loc++;
            locbuf[li] = '\0';
            kprintf("Redirect %d -> %s\n", (uint64_t)status, locbuf);
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
            return bodylen;
        }
        if (total > bufsize - 1) total = bufsize - 1;
        memmove(buf, raw, total);
        buf[total] = '\0';
        return total;
    }
    kprintf("Too many redirects\n");
    return -1;
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

/* ── DNS cache public API ────────────────────────────────────────── */

void net_dns_cache_set(const char *hostname, uint32_t ip) {
    dns_cache_store(hostname, ip);
}

uint32_t net_dns_cache_get(const char *hostname) {
    return dns_cache_lookup(hostname);
}

void net_dns_cache_clear(void) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        dns_cache[i].valid = 0;
    }
}
