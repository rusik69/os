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
static uint32_t dhcp_xid = 0x12345678;
static uint32_t dhcp_offered_ip = 0;
static uint32_t dhcp_server_ip = 0;
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
        if (code == 6 && olen >= 4) net_dns_server = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) | ((uint32_t)opt[2] << 8) | opt[3];
        if (code == 54 && olen >= 4) server_id = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) | ((uint32_t)opt[2] << 8) | opt[3];
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
        if (router) net_gateway_ip = router;
        if (mask) net_subnet_mask = mask;
        dhcp_state = 3;
        net_dhcp_done = 1;
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
        while (pos < len) {
            uint8_t lbl = data[pos];
            if (lbl == 0) { pos++; break; }
            if ((lbl & 0xC0) == 0xC0) { pos += 2; break; }
            pos += lbl + 1;
        }
        pos += 4;
    }

    for (uint16_t a = 0; a < ancount && pos + 12 <= len; a++) {
        if ((data[pos] & 0xC0) == 0xC0) pos += 2;
        else { while (pos < len && data[pos] != 0) { if ((data[pos] & 0xC0) == 0xC0) { pos += 2; goto got_name; } pos += data[pos] + 1; } pos++; }
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

void handle_udp(struct ip_header *ip_hdr, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct udp_header)) return;
    struct udp_header *udp = (struct udp_header *)payload;
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t udp_len = ntohs(udp->length);
    if (udp_len > len) return;
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
}

void net_udp_bind(uint16_t port, udp_recv_handler handler) {
    if (net_num_udp_bindings >= MAX_UDP_BINDINGS) return;
    net_udp_bindings[net_num_udp_bindings].port = port;
    net_udp_bindings[net_num_udp_bindings].handler = handler;
    net_num_udp_bindings++;
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
        volatile uint32_t tries = 0;
        while (!dns_reply_received) {
            net_poll();
            tries++;
            uint64_t now = timer_get_ticks();
            if (now != start && now - start > 200) break;
            if (tries > 2000000) break;
        }
    }
    return dns_result_ip;
}

void net_dhcp_discover(void) {
    kprintf("[..] DHCP: Sending DISCOVER...\n");
    dhcp_send_discover();

    /* Poll for DHCP completion; resend discover only if still in state 0-1 */
    volatile uint32_t total = 0;
    int resends = 0;
    while (dhcp_state != 3 && total < 30000000) {
        net_poll();
        total++;
        /* Resend discover every 5M iterations if no offer received yet */
        if (dhcp_state <= 1 && total % 5000000 == 0 && resends < 2) {
            dhcp_send_discover();
            resends++;
        }
    }

    if (dhcp_state != 3) {
        kprintf("[!!] DHCP: Timeout, using fallback IP\n");
        net_our_ip = (192U << 24) | (168U << 16) | (64U << 8) | 15U;
        net_gateway_ip = (192U << 24) | (168U << 16) | (64U << 8) | 1U;
        net_subnet_mask = (255U << 24) | (255U << 16) | (255U << 8) | 0U;
        net_dns_server = net_gateway_ip;
    }

    if (dhcp_state == 3)
        kprintf("[OK] DHCP: %u.%u.%u.%u\n",
            (uint64_t)((net_our_ip >> 24) & 0xFF), (uint64_t)((net_our_ip >> 16) & 0xFF),
            (uint64_t)((net_our_ip >> 8) & 0xFF), (uint64_t)(net_our_ip & 0xFF));

    if (net_gateway_ip)
        arp_resolve_gateway();
}

int net_http_get(const char *host, uint16_t port, const char *path,
                 char *buf, int bufsize) {
    uint32_t ip = net_dns_resolve(host);
    if (!ip) return -1;

    int conn = net_tcp_connect(ip, port);
    if (conn < 0) return -1;

    char req[512];
    int rlen = 0;
    const char *method = "GET ";
    while (*method) req[rlen++] = *method++;
    if (!path || !*path) req[rlen++] = '/';
    else { const char *pp = path; while (*pp) req[rlen++] = *pp++; }
    const char *ver = " HTTP/1.0\r\nHost: ";
    while (*ver) req[rlen++] = *ver++;
    const char *h = host;
    while (*h) req[rlen++] = *h++;
    const char *end = "\r\nConnection: close\r\n\r\n";
    while (*end) req[rlen++] = *end++;

    net_tcp_send(conn, req, rlen);

    int total = 0;
    while (total < bufsize - 1) {
        int n = net_tcp_recv(conn, buf + total, bufsize - 1 - total, 300);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    net_tcp_close(conn);

    char *body = 0;
    for (int i = 0; i < total - 3; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            body = buf + i + 4;
            break;
        }
    }
    if (body) {
        int bodylen = total - (body - buf);
        memmove(buf, body, bodylen);
        buf[bodylen] = '\0';
        return bodylen;
    }
    return total;
}
