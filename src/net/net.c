/* net.c — Core networking: state, ethernet/IP, ARP, ICMP, poll, init */

#include "net_internal.h"
#include "e1000.h"
#include "string.h"
#include "printf.h"
#include "timer.h"

/* Shared network state */
uint8_t  net_our_mac[6];
uint32_t net_our_ip;
uint32_t net_gateway_ip;
uint32_t net_subnet_mask;
int      net_dhcp_done = 0;
uint32_t net_dns_server = 0;
uint8_t  net_gw_mac[6];
int      net_gw_mac_known = 0;
uint16_t net_ip_id_counter = 1;

/* ARP cache */
struct arp_entry net_arp_cache[ARP_CACHE_SIZE];

/* TCP connection table */
struct tcp_conn tcp_conns[MAX_TCP_CONNS];

/* Listeners */
struct tcp_listener net_listeners[MAX_LISTENERS];
int net_num_listeners = 0;

/* UDP bindings */
struct udp_binding net_udp_bindings[MAX_UDP_BINDINGS];
int net_num_udp_bindings = 0;

/* Packet receive buffer */
static uint8_t pkt_buf[2048];

/* ICMP ping state */
static volatile int ping_reply_received = 0;

/* --- ARP cache --- */

void arp_cache_add(uint32_t ip, const uint8_t *mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (net_arp_cache[i].valid && net_arp_cache[i].ip == ip) {
            memcpy(net_arp_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!net_arp_cache[i].valid) {
            net_arp_cache[i].ip = ip;
            memcpy(net_arp_cache[i].mac, mac, 6);
            net_arp_cache[i].valid = 1;
            return;
        }
    }
    net_arp_cache[0].ip = ip;
    memcpy(net_arp_cache[0].mac, mac, 6);
    net_arp_cache[0].valid = 1;
}

uint8_t *arp_cache_lookup(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (net_arp_cache[i].valid && net_arp_cache[i].ip == ip)
            return net_arp_cache[i].mac;
    }
    return NULL;
}

/* Send an ARP request for the given IP */
static void arp_send_request(uint32_t target_ip) {
    struct arp_packet arp;
    arp.hw_type = htons(1);
    arp.proto_type = htons(ETH_TYPE_IP);
    arp.hw_len = 6;
    arp.proto_len = 4;
    arp.opcode = htons(1);
    memcpy(arp.sender_mac, net_our_mac, 6);
    arp.sender_ip = htonl(net_our_ip);
    memset(arp.target_mac, 0, 6);
    arp.target_ip = htonl(target_ip);
    uint8_t bc[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    send_eth(bc, ETH_TYPE_ARP, &arp, sizeof(arp));
}

/* Resolve gateway MAC via ARP with retries + polling */
void arp_resolve_gateway(void) {
    if (net_gw_mac_known || !net_gateway_ip) return;
    for (int a = 0; a < 3 && !net_gw_mac_known; a++) {
        arp_send_request(net_gateway_ip);
        volatile uint32_t w = 0;
        while (!net_gw_mac_known && w < 2000000) { net_poll(); w++; }
    }
}

/* Resolve an arbitrary local IP via ARP with retries + polling */
static void arp_resolve_ip(uint32_t ip) {
    if (arp_cache_lookup(ip)) return;
    for (int a = 0; a < 3 && !arp_cache_lookup(ip); a++) {
        arp_send_request(ip);
        volatile uint32_t w = 0;
        while (!arp_cache_lookup(ip) && w < 2000000) { net_poll(); w++; }
    }
}

/* --- Checksum --- */

uint16_t net_checksum(const void *data, int len) {
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len == 1) sum += *(const uint8_t *)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~(uint16_t)sum;
}

/* --- IP getters/setters --- */

void net_get_ip(uint8_t *ip) {
    ip[0] = (net_our_ip >> 24) & 0xFF;
    ip[1] = (net_our_ip >> 16) & 0xFF;
    ip[2] = (net_our_ip >> 8) & 0xFF;
    ip[3] = net_our_ip & 0xFF;
}

uint32_t net_get_gateway(void) { return net_gateway_ip; }
uint32_t net_get_mask(void)    { return net_subnet_mask; }

void net_set_ip(uint32_t ip, uint32_t gw, uint32_t mask) {
    net_our_ip = ip;
    net_gateway_ip = gw;
    net_subnet_mask = mask;
}

/* --- Ethernet/IP send --- */

static volatile int send_ip_resolving = 0;  /* prevent recursive ARP resolve */

void send_eth(const uint8_t *dst_mac, uint16_t type, const void *payload, uint16_t len) {
    static uint8_t frame[1518];
    struct eth_header *eth = (struct eth_header *)frame;
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, net_our_mac, 6);
    eth->type = htons(type);
    memcpy(frame + sizeof(struct eth_header), payload, len);
    e1000_send(frame, sizeof(struct eth_header) + len);
}

void send_ip(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t len) {
    static uint8_t buf[1500];
    struct ip_header *ip = (struct ip_header *)buf;
    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = 0x45;
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->src_ip = htonl(net_our_ip);
    ip->dst_ip = htonl(dst_ip);
    ip->total_len = htons(sizeof(struct ip_header) + len);
    ip->id = htons(net_ip_id_counter++);
    ip->checksum = 0;
    memcpy(buf + sizeof(struct ip_header), payload, len);
    ip->checksum = net_checksum(ip, sizeof(struct ip_header));

    uint8_t *dst_mac;
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    int on_local = 0;
    if (net_subnet_mask && ((dst_ip & net_subnet_mask) == (net_our_ip & net_subnet_mask)))
        on_local = 1;
    if (dst_ip == 0xFFFFFFFF)
        on_local = 0;

    if (on_local) {
        uint8_t *cached = arp_cache_lookup(dst_ip);
        if (!cached && !send_ip_resolving) {
            send_ip_resolving = 1;
            arp_resolve_ip(dst_ip);
            send_ip_resolving = 0;
            cached = arp_cache_lookup(dst_ip);
        }
        dst_mac = cached ? cached : bcast;
    } else {
        if (!net_gw_mac_known && !send_ip_resolving) {
            send_ip_resolving = 1;
            arp_resolve_gateway();
            send_ip_resolving = 0;
        }
        dst_mac = net_gw_mac_known ? net_gw_mac : bcast;
    }

    send_eth(dst_mac, ETH_TYPE_IP, buf, sizeof(struct ip_header) + len);
}

/* --- ARP handler --- */

static void handle_arp(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct arp_packet)) return;
    struct arp_packet *arp = (struct arp_packet *)data;

    uint32_t target = ntohl(arp->target_ip);
    uint32_t sender = ntohl(arp->sender_ip);

    if (sender) {
        arp_cache_add(sender, arp->sender_mac);
    }

    if (sender == net_gateway_ip) {
        memcpy(net_gw_mac, arp->sender_mac, 6);
        net_gw_mac_known = 1;
    }

    if (ntohs(arp->opcode) == 1 && target == net_our_ip) {
        struct arp_packet reply;
        reply.hw_type = htons(1);
        reply.proto_type = htons(ETH_TYPE_IP);
        reply.hw_len = 6;
        reply.proto_len = 4;
        reply.opcode = htons(2);
        memcpy(reply.sender_mac, net_our_mac, 6);
        reply.sender_ip = htonl(net_our_ip);
        memcpy(reply.target_mac, arp->sender_mac, 6);
        reply.target_ip = arp->sender_ip;
        send_eth(arp->sender_mac, ETH_TYPE_ARP, &reply, sizeof(reply));
    }

    if (ntohs(arp->opcode) == 2 && sender == net_gateway_ip) {
        memcpy(net_gw_mac, arp->sender_mac, 6);
        net_gw_mac_known = 1;
    }
}

/* --- ICMP handler --- */

static void handle_icmp(struct ip_header *ip, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct icmp_header)) return;
    struct icmp_header *icmp = (struct icmp_header *)payload;

    if (icmp->type == 8) {
        uint8_t reply_buf[1500];
        memcpy(reply_buf, payload, len);
        struct icmp_header *reply = (struct icmp_header *)reply_buf;
        reply->type = 0;
        reply->checksum = 0;
        reply->checksum = net_checksum(reply_buf, len);
        send_ip(ntohl(ip->src_ip), IP_PROTO_ICMP, reply_buf, len);
    } else if (icmp->type == 0) {
        ping_reply_received = 1;
    }
}

/* --- IP dispatcher --- */

static void handle_ip(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct ip_header)) return;
    struct ip_header *ip = (struct ip_header *)data;

    uint16_t total = ntohs(ip->total_len);
    if (total > len) return;

    uint16_t ihl = (ip->version_ihl & 0xF) * 4;
    const uint8_t *payload = data + ihl;
    uint16_t payload_len = total - ihl;

    if (ip->protocol == IP_PROTO_ICMP)
        handle_icmp(ip, payload, payload_len);
    else if (ip->protocol == IP_PROTO_TCP)
        handle_tcp(ip, payload, payload_len);
    else if (ip->protocol == IP_PROTO_UDP)
        handle_udp(ip, payload, payload_len);
}

/* --- Ping --- */

int net_ping(uint32_t target_ip) {
    uint8_t buf[64];
    struct icmp_header *icmp = (struct icmp_header *)buf;
    icmp->type = 8;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(0x1234);
    icmp->seq = htons(1);
    for (int i = 0; i < 32; i++)
        buf[sizeof(struct icmp_header) + i] = (uint8_t)i;
    icmp->checksum = net_checksum(buf, sizeof(struct icmp_header) + 32);

    ping_reply_received = 0;
    send_ip(target_ip, IP_PROTO_ICMP, buf, sizeof(struct icmp_header) + 32);

    uint64_t start = timer_get_ticks();
    volatile uint32_t tries = 0;
    while (!ping_reply_received) {
        net_poll();
        tries++;
        uint64_t now = timer_get_ticks();
        if (now != start && now - start > 300) return -1;
        if (tries > 3000000) return -1;
    }
    uint64_t elapsed = timer_get_ticks() - start;
    return (int)(elapsed * 10);
}

/* --- Poll --- */

void net_poll(void) {
    int len = e1000_receive(pkt_buf, sizeof(pkt_buf));
    if (len <= 0) return;
    if (len < (int)sizeof(struct eth_header)) return;

    struct eth_header *eth = (struct eth_header *)pkt_buf;
    uint16_t type = ntohs(eth->type);
    const uint8_t *payload = pkt_buf + sizeof(struct eth_header);
    uint16_t payload_len = len - sizeof(struct eth_header);

    if (type == ETH_TYPE_ARP)
        handle_arp(payload, payload_len);
    else if (type == ETH_TYPE_IP) {
        if (payload_len >= sizeof(struct ip_header)) {
            struct ip_header *ip = (struct ip_header *)payload;
            uint32_t src = ntohl(ip->src_ip);
            if (src) arp_cache_add(src, eth->src);
        }
        handle_ip(payload, payload_len);
    }
}

/* --- Init --- */

void net_init(void) {
    net_our_ip = 0;
    net_gateway_ip = 0;
    net_subnet_mask = 0;
    e1000_get_mac(net_our_mac);
    memset(tcp_conns, 0, sizeof(tcp_conns));
    memset(net_listeners, 0, sizeof(net_listeners));
    memset(net_arp_cache, 0, sizeof(net_arp_cache));
    net_num_listeners = 0;
}

/* --- ARP list --- */

int net_arp_list(void (*cb)(uint32_t ip, const uint8_t *mac)) {
    int count = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (net_arp_cache[i].valid) {
            if (cb) cb(net_arp_cache[i].ip, net_arp_cache[i].mac);
            count++;
        }
    }
    return count;
}
