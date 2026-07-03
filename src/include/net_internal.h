#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H

#include "net.h"
#include "types.h"
#include "tcp_cc.h"     /* CC_ALGO_* constants for cc_algo field */
#include "tcp_bbr.h"   /* struct bbr_data for inline embedding in tcp_conn */
#include "tcp_bbr3.h"  /* struct bbr3_data for inline embedding in tcp_conn */
#include "tcp_cubic.h" /* struct cubic_data for inline embedding in tcp_conn */
#include "tcp_newreno.h" /* struct newreno_data for inline embedding in tcp_conn */
#include "tcp_westwood.h" /* struct westwood_data for inline embedding in tcp_conn */
#include "tcp_vegas.h"   /* struct vegas_data for inline embedding in tcp_conn */
#include "tcp_hybla.h"   /* struct hybla_data for inline embedding in tcp_conn */

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

/* Link-layer send/receive — defined in net.c */
int net_link_send(const void *data, uint16_t len);
int net_link_recv(void *buf, uint16_t max_len);
int net_rx_pending(void);

/* IPv6 state — defined in ipv6.c */
extern struct in6_addr net_our_ipv6_ll;   /* link-local address (FE80::/10) */
extern struct in6_addr net_our_ipv6_gua;  /* global unicast (via SLAAC) */
extern int             net_ipv6_ll_ready; /* 1 = link-local address configured */
extern int             net_ipv6_gua_valid;/* 1 = GUA configured via SLAAC */
extern struct in6_addr net_ipv6_gateway;  /* default gateway (from RA) */
extern struct in6_addr net_ipv6_dns;      /* DNS server (from RDNSS) */
extern uint32_t        net_ipv6_ns_count; /* NS counter for NDP */

/* ── IPv6 address management ──────────────────────────────────── */
#define IPV6_ADDR_TABLE_SIZE 8

#define IPV6_ADDR_STATE_TENTATIVE  0  /* DAD in progress */
#define IPV6_ADDR_STATE_PREFERRED  1  /* usable for all communication */
#define IPV6_ADDR_STATE_DEPRECATED 2  /* preferred lifetime expired */
#define IPV6_ADDR_STATE_PERMANENT  3  /* never expires (link-local, loopback) */
#define IPV6_ADDR_STATE_DETACHED   4  /* tentative but failed DAD */

#define IPV6_ADDR_F_AUTOCONF  0x01   /* auto-configured via SLAAC */
#define IPV6_ADDR_F_DAD       0x02   /* DAD completed */

struct ipv6_addr_entry {
    struct in6_addr addr;
    uint8_t prefix_len;
    int     state;              /* IPV6_ADDR_STATE_* */
    int     scope;              /* IPV6_ADDR_SCOPE_* (determined by addr) */
    uint32_t valid_lifetime;    /* seconds remaining (0xFFFFFFFF = infinite) */
    uint32_t preferred_lifetime;
    uint64_t expiry_tick;       /* tick when valid_lifetime expires */
    uint32_t flags;
    int     valid;              /* 1 = slot in use */
};

extern struct ipv6_addr_entry ipv6_addr_table[IPV6_ADDR_TABLE_SIZE];
extern int ipv6_addr_count;

/* Address management API */
int  ipv6_addr_add(const struct in6_addr *addr, uint8_t prefix_len,
                   int state, uint32_t valid_lifetime,
                   uint32_t preferred_lifetime, uint32_t flags);
int  ipv6_addr_del(const struct in6_addr *addr);
struct ipv6_addr_entry *ipv6_addr_find(const struct in6_addr *addr);
struct ipv6_addr_entry *ipv6_addr_find_by_state(int state);
struct ipv6_addr_entry *ipv6_addr_select_source(const struct in6_addr *dst);
int  ipv6_addr_get_scope(const struct in6_addr *addr);
int  ipv6_addr_is_ours(const struct in6_addr *addr);
int  ipv6_addr_get_ll(struct in6_addr *out);
int  ipv6_addr_get_gua(struct in6_addr *out);
void ipv6_addr_dump(void);

/* ── ARP cache with timeout and retry ───────────────────────────── */
#define ARP_CACHE_SIZE 16

/* ARP entry timeout: 300 seconds (5 minutes) since last confirmation
 * At TIMER_FREQ=100 Hz: 300 * 100 = 30000 ticks */
#define ARP_TIMEOUT_TICKS       30000

/* ARP retry interval: 1 second between probes = 100 ticks */
#define ARP_RETRY_INTERVAL_TICKS 100

/* Max ARP resolution retries before declaring unreachable */
#define ARP_MAX_RETRIES         3

struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;          /* 1 = entry has a known MAC */
    uint64_t last_seen_tick; /* tick when MAC was last confirmed */
    int      retry_count;    /* consecutive resolution failures */
    int      resolving;      /* 1 = actively trying to resolve this IP */
    uint64_t last_probe_tick;/* tick when last ARP probe was sent */
};

extern struct arp_entry net_arp_cache[ARP_CACHE_SIZE];

/* ── Pending ARP resolution queue ───────────────────────────────── */
#define ARP_PENDING_QUEUE_SIZE 8
#define ARP_PENDING_MAX_PKT    1522  /* max Ethernet frame size */

struct arp_pending_pkt {
    uint32_t target_ip;               /* IP we're trying to resolve */
    uint8_t  data[ARP_PENDING_MAX_PKT]; /* buffered frame */
    uint16_t len;                      /* length of buffered frame */
    int      in_use;                   /* slot active */
    uint64_t enqueue_tick;            /* when this was queued */
};

extern struct arp_pending_pkt arp_pending_queue[ARP_PENDING_QUEUE_SIZE];

/* ── ARP API ────────────────────────────────────────────────────── */
void     arp_cache_add(uint32_t ip, const uint8_t *mac);
uint8_t *arp_cache_lookup(uint32_t ip);
void     arp_resolve_gateway(void);
void     arp_send_request(uint32_t target_ip);

/* Enhanced operations */
int      arp_resolve_or_queue(uint32_t dst_ip,
                              const void *frame, uint16_t frame_len);
void     arp_gc(void);                  /* call periodically from net_poll */
void     arp_retry_pending(void);       /* retry unresolved entries */
void     arp_flush_pending(uint32_t ip);/* flush queued packets for resolved IP */
int      arp_pending_count(void);       /* number of queued frames */

/* TCP connection table */
#define MAX_TCP_CONNS 16

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
    TCP_CLOSING,  /* simultaneous close — both sides FIN */
};

struct tcp_sack_block {
    uint32_t left;   /* SACK block left edge (seq number) */
    uint32_t right;  /* SACK block right edge (seq number) */
};

#define TCP_MAX_SACK_BLOCKS 4

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
    uint32_t urg_seq;            /* last urgent sequence number (TCP URG handling) */
    /* Congestion control (Reno: slow start / AIMD / fast recovery) */
    uint32_t cwnd;              /* congestion window in segments */
    uint32_t ssthresh;          /* slow-start threshold */
    uint8_t  dupack_count;      /* duplicate ACK counter for fast recovery */
    /* RTT estimation (Jacobson's algorithm) */
    int32_t  srtt;              /* smoothed round-trip time (scaled by 8) */
    int32_t  rttvar;            /* round-trip time variation (scaled by 4) */
    /* Retransmission */
    uint8_t  tx_unacked_buf[4096];  /* copy of last sent data awaiting ACK */
    uint16_t tx_unacked_len;        /* bytes currently unacked (0 = nothing pending) */
    uint32_t tx_unacked_seq;        /* seq number of first unacked byte */
    uint64_t last_send_tick;        /* tick when segment was last sent/retransmitted */
    uint8_t  retrans_count;         /* number of retransmit attempts so far */
    uint16_t rto;                   /* retransmit timeout in ticks (30 = 3000ms default) */
    /* Last ACK received (for duplicate ACK detection) */
    uint32_t last_ack;
    /* SACK */
    struct tcp_sack_block sack_blocks[TCP_MAX_SACK_BLOCKS];
    int     sack_pending;           /* 1 if we are waiting for SACK-permitted */
    /* Socket options */
    int     tcp_nodelay;            /* 1 = disable Nagle's algorithm */
    int     tcp_cork;               /* 1 = buffer until uncorked */
    int     keepalive;              /* 1 = send keepalive probes */
    uint64_t last_activity_tick;    /* last data rx/tx tick (for keepalive) */
    uint16_t keepalive_interval;    /* ticks between keepalive probes (default ~5s=500) */
    uint8_t  keepalive_probes;      /* probes sent without reply */
    uint8_t  keepalive_probes_max;  /* max probes before disconnect (default 3) */
    /* TCP MD5 signature option (kind 19) */
    int     md5_enabled;            /* 1 = MD5 signature enabled on this connection */
    uint8_t md5_digest[16];         /* MD5 digest (placeholder — not computed) */
    /* TCP Fast Open (kind 34) */
    int     tfo_cookie_present;     /* 1 if TFO cookie was received */
    uint8_t tfo_cookie[8];          /* TFO cookie value */
    /* CUBIC congestion control state (embedded struct) */
    struct cubic_data cubic;
    /* RACK (Recent ACKnowledgment) loss detection — Item 156 */
    uint32_t rack_fwd_mark;         /* highest seq delivered (ACKed or SACKed) */
    uint64_t rack_fwd_tick;         /* tick when fwd_mark was last updated */
    uint32_t rack_reo_wnd;          /* reordering window in ticks (min RTT/4) */
    uint32_t rack_min_rtt;          /* minimum observed RTT in ticks */
    /* Nagle / Delayed ACK */
    uint8_t  delayed_ack_pending;   /* 1 if we have a pending delayed ACK */
    uint64_t delayed_ack_tick;      /* tick when delayed ACK timer was started */
    uint8_t  nagle_buf[4096];       /* Nagle accumulation buffer for small writes */
    uint16_t nagle_buf_len;         /* bytes accumulated in Nagle buffer */
    /* TIME_WAIT tracking */
    uint64_t time_wait_deadline;    /* tick when TIME_WAIT expires (2*MSL) */

    /* ── PRR (Proportional Rate Reduction, RFC 6937) — Item 158 ──────
     * During fast recovery, PRR regulates the number of segments sent
     * per ACK to be proportional to the number of bytes delivered.
     * This reduces burstiness and avoids excess window reductions. */
    uint32_t prr_delivered;         /* total bytes delivered (newly ACKed) during recovery */
    uint32_t prr_out;               /* total bytes sent during recovery */
    uint8_t  in_recovery;           /* 1 if currently in fast recovery (PRR active) */

    /* ── Congestion control algorithm selection ───────────────────────
     * 0 = CUBIC (default), 1 = BBR, 2 = BBRv3, 3 = NewReno,
     * 4 = Westwood, 5 = Vegas, 6 = Hybla, 7 = Illinois,
     * 8 = BIC, 9 = BBRv2
     * Set via setsockopt(TCP_CONGESTION) or sysctl.
     * See src/include/tcp_cc.h for the CC_ALGO_* constants. */
    uint8_t  cc_algo;

    /* ── NewReno fast retransmit + fast recovery state ─────────────────
     * Only valid when cc_algo == 3. */
    struct newreno_data newreno;

    /* ── BBR congestion control state ─────────────────────────────────
     * Only valid when cc_algo == 1. */
    struct bbr_data bbr;

    /* ── BBRv3 congestion control state ────────────────────────────────
     * Only valid when cc_algo == 2 (BBRv3 with ECN support). */
    struct bbr3_data bbr3;

    /* ── TCP Westwood+ congestion control state ─────────────────────────
     * Bandwidth-estimation based CC for lossy links. */
    struct westwood_data westwood;

    /* ── TCP Vegas congestion control state ─────────────────────────────
     * Delay-based congestion avoidance (Brakmo & Peterson, 1995). */
    struct vegas_data vegas;

    /* ── TCP Hybla congestion control state ──────────────────────────────
     * RTT-normalised congestion control for satellite links. */
    struct hybla_data hybla;

    /* ── IPv6 flow label (RFC 6437) ───────────────────────────────────
     * Flow label is a 20-bit value identifying packets belonging to the
     * same flow.  Computed once per TCP connection as a pseudo-random
     * hash of the 5-tuple (src_addr, dst_addr, src_port, dst_port,
     * protocol).  0 = not set / IPv4-only connection. */
    uint32_t flow_label;
};

extern struct tcp_conn tcp_conns[MAX_TCP_CONNS];

/* TCP listener */
#define ACCEPT_QUEUE_SIZE 8
struct tcp_listener {
    uint16_t port;
    tcp_connect_handler on_connect;
    tcp_data_handler on_data;
    tcp_close_handler on_close;
    /* Accept queue — used when callbacks are NULL (userspace accept model) */
    int accept_queue[ACCEPT_QUEUE_SIZE];
    int accept_head;
    int accept_tail;
    int accept_count;
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

/* IPv6 protocol handlers (called from net.c net_poll) */
void handle_ipv6(const uint8_t *data, uint16_t len);
void handle_icmpv6(struct ipv6_header *ip6, const uint8_t *payload, uint16_t len);

/* IPv6 send helpers */
void send_ipv6(const struct in6_addr *dst, uint8_t next_hdr,
               const void *payload, uint16_t len);
void send_ipv6_flow(const struct in6_addr *dst, uint8_t next_hdr,
                    const void *payload, uint16_t len,
                    uint32_t flow_label);
void send_eth_ipv6(const uint8_t *dst_mac, const void *payload, uint16_t len);

/* IPv6 flow label (RFC 6437) — compute a 20-bit flow label from the
 * transport-layer 5-tuple plus a secret seed.  Returns value masked
 * to IPV6_FLOW_LABEL_MASK. */
#define IPV6_FLOW_LABEL_MASK 0x000FFFFFU
void ipv6_flow_label_init(void);
uint32_t ipv6_flow_label_calc(const struct in6_addr *src,
                               const struct in6_addr *dst,
                               uint16_t src_port, uint16_t dst_port,
                               uint8_t protocol);

/* IPv6 internal helpers (ipv6_core.c) */
int  ipv6_parse_exthdr(const uint8_t *data, uint16_t total_len,
                        uint8_t *out_proto,
                        const uint8_t **out_payload,
                        uint16_t *out_payload_len,
                        const struct ipv6_fragment **out_frag_hdr);
void handle_ipv6_packet(const uint8_t *data, uint16_t total_len);
struct ipv6_frag_stats {
    uint32_t rx_fragments;
    uint32_t rx_reassembled;
    uint32_t rx_timed_out;
    uint32_t rx_dropped;
    uint32_t rx_oom;
};
void ipv6_frag_stats_get(struct ipv6_frag_stats *out);

/* ── IPv6 fragmentation (RFC 8200 §4.5) ────────────────────────── */

/* Fragment reassembly slot count and buffer size */
#define IPV6_FRAG_SLOTS       8    /* max concurrent fragmented IPv6 datagrams */
#define IPV6_FRAG_BUF_SIZE    4096 /* max reassembly buf */
#define IPV6_FRAG_TTL_TICKS   3000 /* ~30 seconds at 100 Hz timer */

/* Send fragmented IPv6 datagram */
void send_ipv6_fragmented(const struct in6_addr *dst, uint8_t next_hdr,
                          const void *payload, uint16_t len,
                          uint32_t identification, uint16_t effective_mtu);

/* Poll for expired fragment slots (call from net_poll or timer) */
void ipv6_frag_poll(void);

/* Send an atomic fragment (Fragment Header with offset=0, M=0) */
void send_ipv6_atomic(const struct in6_addr *dst, uint8_t next_hdr,
                      const void *payload, uint16_t len,
                      uint32_t identification);

void ipv6_calc_solicited_node(const struct in6_addr *addr, struct in6_addr *mcast);
void ipv6_eui64_from_mac(const uint8_t *mac, struct in6_addr *out);
int  ipv6_addr_is_multicast(const struct in6_addr *addr);
int  ipv6_addr_is_linklocal(const struct in6_addr *addr);
int  ipv6_addr_is_unspecified(const struct in6_addr *addr);
int  ipv6_addr_equal(const struct in6_addr *a, const struct in6_addr *b);
uint16_t ipv6_checksum(const struct in6_addr *src, const struct in6_addr *dst,
                        uint8_t next_hdr, const void *data, uint16_t data_len);
void ipv6_nd_cache_add(const struct in6_addr *ip6, const uint8_t *mac);

/* IPv6 NDISC (Neighbor Discovery) module — ipv6_ndisc.c */
void    ipv6_nd_init(void);
void    ipv6_nd_poll(void);
int     ipv6_nd_send_ns(const struct in6_addr *target);
int     ipv6_nd_send_na(const struct in6_addr *target,
                        const struct in6_addr *dst,
                        int solicited, int override,
                        const struct in6_addr *src_override);
int     ipv6_nd_send_rs(void);
int     ipv6_nd_send_ra(const struct in6_addr *dst);
void    ipv6_nd_handle_ns(struct ipv6_header *ip6,
                          const uint8_t *payload, uint16_t len);
void    ipv6_nd_handle_na(struct ipv6_header *ip6,
                          const uint8_t *payload, uint16_t len);
void    ipv6_nd_handle_rs(struct ipv6_header *ip6,
                          const uint8_t *payload, uint16_t len);
void    ipv6_nd_handle_ra(struct ipv6_header *ip6,
                          const uint8_t *payload, uint16_t len);
uint8_t *ipv6_nd_cache_lookup(const struct in6_addr *ip6);
void    ipv6_nd_cache_dump(void);

/* ── DAD (Duplicate Address Detection) — RFC 4862 §5.4 ────────── */
void    ipv6_dad_start(const struct in6_addr *addr);
void    ipv6_dad_poll(void);
void    ipv6_dad_conflict(const struct in6_addr *addr);

/* ── IPv6 Path MTU Discovery (RFC 1981) — ipv6_pmtu.c ─────────── */
#define IPV6_DEFAULT_LINK_MTU   1460   /* 1500 - 40 (IPv6 hdr) */
#define IPV6_MIN_MTU            1280   /* RFC 8200 §5 minimum */
void    ipv6_pmtu_init(void);
uint16_t ipv6_pmtu_lookup(const struct in6_addr *dst);
void    ipv6_pmtu_update(const struct in6_addr *dst, uint16_t pmtu);
void    ipv6_pmtu_poll(void);
void    ipv6_send_pmtu(const struct in6_addr *src,
                       const struct in6_addr *dst,
                       const struct ipv6_header *offending,
                       uint16_t offending_len,
                       uint32_t next_hop_mtu);

/* TCP internal helpers (net_tcp.c) */
void send_tcp(struct tcp_conn *conn, uint8_t flags, const void *data, uint16_t data_len);
void net_tcp_check_retransmit(void);
void net_tcp_check_keepalive(void);
uint16_t net_transport_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                                 const void *data, uint16_t data_len);

/* TCP over IPv6 (tcp_ipv6.c) */
void tcp_ipv6_init(void);
void tcp_ipv6_compute_flow_label(struct tcp_conn *conn,
                                  const struct in6_addr *src_addr,
                                  const struct in6_addr *dst_addr);
void send_tcp_ipv6(struct tcp_conn *conn,
                    const struct in6_addr *dst_addr,
                    uint8_t flags,
                    const void *data, uint16_t data_len);
void handle_tcp_ipv6(struct ipv6_header *ip6,
                      const uint8_t *payload, uint16_t len);

/* IP routing table (net.c) */
#define RT_MAX_ENTRIES 16
struct rt_entry {
    uint32_t dst;
    uint32_t mask;
    uint32_t gw;
    int      iface;
};
extern struct rt_entry rt_table[RT_MAX_ENTRIES];
extern int rt_num_entries;
int  rt_add(uint32_t dst, uint32_t mask, uint32_t gw, int iface);
int  rt_del(uint32_t dst, uint32_t mask);
int  rt_lookup(uint32_t ip, uint32_t *gw_out, int *iface_out);
void rt_flush(void);

/* ARP gratuitous announcement (net.c) */
void arp_announce(void);

/* ICMP destination unreachable (net_udp.c) */
void icmp_send_unreachable(uint32_t dst, uint32_t src, uint8_t *orig_pkt, uint16_t orig_len);

/* ICMP Time Exceeded (net_udp.c) — TTL expired in transit */
void icmp_send_timeexceeded(uint32_t dst, uint32_t src, uint8_t *orig_pkt, uint16_t orig_len);

/* Initialize ICMP rate limit sysctls (net_udp.c) */
void icmp_ratelimit_sysctl_init(void);

/* ── IP fragment reassembly statistics ───────────────────────────── */

/* Fragment reassembly statistics — populated by net.c */
struct frag_stats {
    uint32_t rx_fragments;     /* total fragment packets received */
    uint32_t rx_reassembled;   /* datagrams successfully reassembled */
    uint32_t rx_timed_out;     /* incomplete datagrams expired */
    uint32_t rx_dropped;       /* fragments dropped due to errors */
    uint32_t rx_overlaps;      /* overlapping fragments rejected (attack) */
    uint32_t rx_oom;           /* fragments dropped due to no free slot */
    uint32_t active_slots;     /* current number of active fragment slots */
    uint32_t max_active;       /* peak active slot count since boot */
};

/* Copy current fragment statistics to caller-supplied buffer */
void net_frag_stats(struct frag_stats *out);

#endif /* NET_INTERNAL_H */
