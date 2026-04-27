#ifndef NET_H
#define NET_H

#include "types.h"

/* Byte order helpers */
static inline uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000U);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* Default network config (QEMU user-mode networking) */
#define NET_IP       0x0A000F02  /* 10.0.2.15 (big-endian built at runtime) */
#define NET_GATEWAY  0x0A000202  /* 10.0.2.2 */
#define NET_MASK     0xFFFFFF00  /* 255.255.255.0 */

#define ETH_TYPE_IP   0x0800
#define ETH_TYPE_ARP  0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DNS_PORT 53

/* Ethernet header */
struct eth_header {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed));

/* ARP packet */
struct arp_packet {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t opcode;
    uint8_t  sender_mac[6];
    uint32_t sender_ip;
    uint8_t  target_mac[6];
    uint32_t target_ip;
} __attribute__((packed));

/* IPv4 header */
struct ip_header {
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed));

/* ICMP header */
struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

/* UDP header */
struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

/* TCP header */
struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_off;   /* upper 4 bits = offset in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* TCP pseudo header for checksum */
struct tcp_pseudo {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_len;
} __attribute__((packed));

void net_init(void);
void net_poll(void);
uint16_t net_checksum(const void *data, int len);
void net_get_ip(uint8_t *ip);
uint32_t net_get_gateway(void);
uint32_t net_get_mask(void);
void net_dhcp_discover(void);
int net_ping(uint32_t target_ip);
void net_set_ip(uint32_t ip, uint32_t gw, uint32_t mask);

/* Callbacks for TCP data - called by net stack */
typedef void (*tcp_data_handler)(int conn_id, const void *data, uint16_t len);
typedef void (*tcp_connect_handler)(int conn_id);
typedef void (*tcp_close_handler)(int conn_id);

void net_tcp_listen(uint16_t port, tcp_connect_handler on_connect,
                    tcp_data_handler on_data, tcp_close_handler on_close);
int net_tcp_send(int conn_id, const void *data, uint16_t len);
void net_tcp_close(int conn_id);

/* DNS resolver - returns IP in host byte order, 0 on failure */
uint32_t net_dns_resolve(const char *hostname);

/* Outgoing TCP client */
int net_tcp_connect(uint32_t ip, uint16_t port);
int net_tcp_recv(int conn_id, void *buf, uint16_t bufsize, int timeout_ticks);

/* UDP send */
void net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                  const void *data, uint16_t len);

/* UDP receive binding - register a callback for packets arriving on port */
typedef void (*udp_recv_handler)(uint32_t src_ip, uint16_t src_port,
                                  const uint8_t *data, uint16_t len);
void net_udp_bind(uint16_t port, udp_recv_handler handler);

/* HTTP client - fetches URL, writes body to buf. Returns bytes written or -1 */
int net_http_get(const char *host, uint16_t port, const char *path,
                 char *buf, int bufsize);

/* ARP cache dump - calls cb(ip, mac) for each valid entry.  Returns count. */
int net_arp_list(void (*cb)(uint32_t ip, const uint8_t *mac));

#endif
