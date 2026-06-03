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
#define ETH_TYPE_IPV6 0x86DD

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17
#define IP_PROTO_ICMPV6 58

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

/* IPv6 address (128 bits, network byte order) */
struct in6_addr {
    uint8_t s6_addr[16];
} __attribute__((packed));

/* IPv6 header (40 bytes) */
struct ipv6_header {
    uint32_t vcl_flow;          /* version(4), traffic_class(8), flow_label(20) */
    uint16_t payload_length;    /* length of payload after this header */
    uint8_t  next_header;       /* next header type (protocol) */
    uint8_t  hop_limit;         /* hop limit */
    struct in6_addr src_ip;     /* source address */
    struct in6_addr dst_ip;     /* destination address */
} __attribute__((packed));

/* ICMPv6 header */
struct icmpv6_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
} __attribute__((packed));

/* ICMPv6 Echo Request / Reply (adds id and seq after checksum) */
struct icmpv6_echo {
    struct icmpv6_header hdr;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

/* ICMPv6 Neighbor Solicitation / Advertisement */
struct nd_neighbor {
    struct icmpv6_header icmp;
    uint32_t reserved;          /* NS: reserved; NA: R/S/O flags in top 3 bits */
    struct in6_addr target;     /* target address */
    /* Options follow (type, len, ...) */
} __attribute__((packed));

/* ICMPv6 Router Solicitation */
struct nd_router_solicit {
    struct icmpv6_header icmp;
    uint32_t reserved;
    /* Options follow */
} __attribute__((packed));

/* ICMPv6 Router Advertisement */
struct nd_router_advert {
    struct icmpv6_header icmp;
    uint8_t  cur_hop_limit;
    uint8_t  flags;
    uint16_t router_lifetime;
    uint32_t reachable_time;
    uint32_t retrans_timer;
    /* Options follow */
} __attribute__((packed));

/* NDP Option header */
struct nd_option {
    uint8_t type;
    uint8_t len;   /* length in units of 8 octets */
    /* data follows */
} __attribute__((packed));

#define ND_OPT_SRC_LLADDR   1   /* source link-layer address */
#define ND_OPT_TGT_LLADDR   2   /* target link-layer address */
#define ND_OPT_PREFIX_INFO  3   /* prefix information */
#define ND_OPT_MTU          5   /* MTU option */

#define ICMPV6_RS           133 /* Router Solicitation */
#define ICMPV6_RA           134 /* Router Advertisement */
#define ICMPV6_NS           135 /* Neighbor Solicitation */
#define ICMPV6_NA           136 /* Neighbor Advertisement */

/* IPv6 multicast addresses (network byte order) */
#define IPV6_ADDR_ALL_NODES     { { 0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } }
#define IPV6_ADDR_ALL_ROUTERS   { { 0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,2 } }
#define IPV6_ADDR_LINKLOCAL_PFX { { 0xFE,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } }

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
void net_rx_signal(void);
int  net_rx_pending(void);
/* Block until a network packet is available (waitqueue-based) */
void net_wait_for_packet(void);
int  net_link_send(const void *data, uint16_t len);
uint16_t net_checksum(const void *data, int len);
void net_get_ip(uint8_t *ip);
uint32_t net_get_gateway(void);
uint32_t net_get_mask(void);
uint32_t net_get_dns(void);
/* Read /etc/resolv.conf and return the first nameserver IP, or 0 */
uint32_t net_resolv_conf_read_first(void);
void net_dhcp_discover(void);
void net_dhcp_renew_if_needed(void);
int net_ping(uint32_t target_ip);
void net_set_ip(uint32_t ip, uint32_t gw, uint32_t mask);

/* IPv6 support */
void ipv6_init(void);
void ipv6_poll(void);
int  ipv6_has_linklocal(void);
void ipv6_get_linklocal(struct in6_addr *addr);
int  ipv6_ping6(const struct in6_addr *target);
void ipv6_send_rs(void);

/* Loopback interface */
int  net_loopback_init(void);
int  net_loopback_send(const void *data, int len);

/* TCP keepalive support */
void net_tcp_set_keepalive(int conn_id, int keepalive);
int  net_tcp_get_keepalive(int conn_id);

/* Return number of bytes available in TCP receive buffer (0 = no data) */
int net_tcp_available(int conn_id);

/* Return 1 if the TCP connection is in ESTABLISHED state (writable) */
int net_tcp_is_connected(int conn_id);

/* Return 1 if the TCP connection is closed or has received FIN */
int net_tcp_has_closed(int conn_id);

/* TCP connection info for netstat */
struct tcp_conn_info {
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    int      state;
    uint32_t cwnd;
    uint32_t ssthresh;
    uint64_t last_send_tick;
    uint8_t  retrans_count;
};
int net_tcp_get_info(int conn_id, struct tcp_conn_info *info);

/* Callbacks for TCP data - called by net stack */
typedef void (*tcp_data_handler)(int conn_id, const void *data, uint16_t len);
typedef void (*tcp_connect_handler)(int conn_id);
typedef void (*tcp_close_handler)(int conn_id);

void net_tcp_listen(uint16_t port, tcp_connect_handler on_connect,
                    tcp_data_handler on_data, tcp_close_handler on_close);
void net_tcp_unlisten(uint16_t port);
int net_tcp_send(int conn_id, const void *data, uint16_t len);
void net_tcp_close(int conn_id);

/* DNS resolver - returns IP in host byte order, 0 on failure */
uint32_t net_dns_resolve(const char *hostname);

/* TCP */
int net_tcp_connect(uint32_t ip, uint16_t port);
void tcp_tfo_init(void);       /* TCP Fast Open initialization */
int net_tcp_recv(int conn_id, void *buf, uint16_t bufsize, int timeout_ticks);

/* Blocking TCP server accept — waits up to timeout_ticks for a new
 * connection on a port registered with net_tcp_listen(..., NULL, NULL, NULL).
 * Returns conn_id >= 0, or -1 on timeout / error. */
int net_tcp_accept(uint16_t port, int timeout_ticks);

/* DNS caching */
#include "dns_cache.h"

/* UDP send */
void net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                  const void *data, uint16_t len);

/* UDP connected-socket fast path — pre-resolved destination MAC.
 * Builds the complete Ethernet/IP/UDP frame and sends directly via
 * send_eth(), bypassing ARP cache lookups inside send_ip(). */
void net_udp_send_cached(const uint8_t *dst_mac, uint32_t dst_ip,
                          uint16_t src_port, uint16_t dst_port,
                          const void *data, uint16_t data_len);

/* UDP receive binding - register a callback for packets arriving on port */
typedef void (*udp_recv_handler)(uint32_t src_ip, uint16_t src_port,
                                  const uint8_t *data, uint16_t len);
void net_udp_bind(uint16_t port, udp_recv_handler handler);

/* UDP server — userspace listen/recv */
int  net_udp_listen(uint16_t port);
int  net_udp_recv(uint16_t port, void *buf, uint16_t bufsize,
                  uint32_t *src_ip_out, uint16_t *src_port_out, int timeout_ticks);
void net_udp_unlisten(uint16_t port);

/* HTTP client - fetches URL, writes body to buf. Returns bytes written or -1 */
int net_http_get(const char *host, uint16_t port, const char *path,
                 char *buf, int bufsize);
/* HTTP GET with optional redirect following (follow_redirects=1 to enable, max 5) */
int net_http_get_ex(const char *host, uint16_t port, const char *path,
                    char *buf, int bufsize, int follow_redirects);

/* ARP cache dump - calls cb(ip, mac) for each valid entry.  Returns count. */
int net_arp_list(void (*cb)(uint32_t ip, const uint8_t *mac));

/* TCP connection list for netstat — calls cb(local_port, remote_ip, remote_port, state) */
void net_conn_list(void (*cb)(uint16_t lport, uint32_t rip, uint16_t rport, int state));

/* UDP listener list — calls cb(port) for each active listener */
void net_udp_list(void (*cb)(uint16_t port));

/* IP forwarding control */
extern int net_ip_forwarding;

/* /proc/net interfaces */
struct net_iface_stats {
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t rx_errors;
    uint64_t rx_drops;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t tx_errors;
    uint64_t tx_drops;
};
extern struct net_iface_stats net_iface_stats;

#endif
