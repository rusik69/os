#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H

#include "net.h"
#include "types.h"

/* Shared network state — defined in net.c */
extern uint8_t  net_our_mac[6];
extern uint32_t net_our_ip;
extern uint32_t net_gateway_ip;
extern uint32_t net_subnet_mask;
extern int      net_dhcp_done;
extern uint32_t net_dns_server;
extern uint8_t  net_gw_mac[6];
extern int      net_gw_mac_known;
extern uint16_t net_ip_id_counter;

/* ARP cache */
#define ARP_CACHE_SIZE 16
struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    int valid;
};
extern struct arp_entry net_arp_cache[ARP_CACHE_SIZE];
void     arp_cache_add(uint32_t ip, const uint8_t *mac);
uint8_t *arp_cache_lookup(uint32_t ip);
void     arp_resolve_gateway(void);

/* TCP connection table */
#define MAX_TCP_CONNS 8

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_CLOSE_WAIT,
};

struct tcp_conn {
    enum tcp_state state;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t our_seq;
    uint32_t their_seq;
    uint16_t their_window;
    uint8_t  rxbuf[4096];
    int      rxlen;
    volatile int rx_fin;
};

extern struct tcp_conn tcp_conns[MAX_TCP_CONNS];

/* TCP listener */
struct tcp_listener {
    uint16_t port;
    tcp_connect_handler on_connect;
    tcp_data_handler on_data;
    tcp_close_handler on_close;
};

#define MAX_LISTENERS 4
extern struct tcp_listener net_listeners[MAX_LISTENERS];
extern int net_num_listeners;

/* UDP bindings */
#define MAX_UDP_BINDINGS 8
struct udp_binding {
    uint16_t port;
    udp_recv_handler handler;
};
extern struct udp_binding net_udp_bindings[MAX_UDP_BINDINGS];
extern int net_num_udp_bindings;

/* Core send functions (net.c) */
void send_eth(const uint8_t *dst_mac, uint16_t type, const void *payload, uint16_t len);
void send_ip(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t len);

/* Protocol handlers (called from net.c net_poll/handle_ip) */
void handle_tcp(struct ip_header *ip_hdr, const uint8_t *payload, uint16_t len);
void handle_udp(struct ip_header *ip_hdr, const uint8_t *payload, uint16_t len);

/* TCP internal helpers (net_tcp.c) */
void send_tcp(struct tcp_conn *conn, uint8_t flags, const void *data, uint16_t data_len);

#endif
