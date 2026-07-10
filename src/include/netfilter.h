#ifndef NETFILTER_H
#define NETFILTER_H

#include "types.h"

/* ── Netfilter hook points (Linux-compatible numbering) ──────────── */
#define NF_INET_PRE_ROUTING  0
#define NF_INET_LOCAL_IN     1
#define NF_INET_FORWARD      2
#define NF_INET_LOCAL_OUT    3
#define NF_INET_POST_ROUTING 4
#define NF_MAX_HOOKS         5

/* Actions returned by hook handlers */
#define NF_ACCEPT 0
#define NF_DROP   1
#define NF_REJECT 2
#define NF_STOLEN   3   /* Packet taken over by hook (don't continue) */
#define NF_QUEUE    4   /* Packet queued for userspace */
#define NF_REPEAT   5   /* Re-run this hook for the packet */

/* Hook function type: return NF_ACCEPT, NF_DROP, or NF_REJECT */
typedef int (*nf_hookfn)(void *skb, int hook);

/* Registered hook entry */
struct nf_hook_entry {
    nf_hookfn fn;
    int priority;
    struct nf_hook_entry *next;
};

/* Packet filter rule */
struct nf_rule {
    uint32_t src_ip;
    uint32_t src_mask;
    uint32_t dst_ip;
    uint32_t dst_mask;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;   /* 0 = any */
    uint8_t  action;     /* NF_ACCEPT, NF_DROP, NF_REJECT */
};

/* ── IP protocols used by conntrack ────────────────────────────────── */
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

/* ── Connection tracking — TCP states ──────────────────────────────── */
#define TCP_CONN_NONE         0
#define TCP_CONN_SYN_SENT     1
#define TCP_CONN_SYN_RECV     2
#define TCP_CONN_ESTABLISHED  3
#define TCP_CONN_FIN_WAIT_1   4
#define TCP_CONN_FIN_WAIT_2   5
#define TCP_CONN_CLOSE_WAIT   6
#define TCP_CONN_CLOSING      7
#define TCP_CONN_LAST_ACK     8
#define TCP_CONN_TIME_WAIT    9
#define TCP_CONN_MAX_STATE   10

/* ── Connection tracking — UDP states ─────────────────────────────── */
#define UDP_CONN_NONE       0
#define UDP_CONN_UNREPLIED  1
#define UDP_CONN_ASSURED    2  /* bidirectional traffic seen */

/* ── Connection tracking — ICMP states ────────────────────────────── */
#define ICMP_CONN_NONE      0
#define ICMP_CONN_REQUEST   1
#define ICMP_CONN_REPLY     2
#define ICMP_CONN_ERROR     3  /* ICMP error relating to another conn */

/* ── Generic conntrack states (compatibility) ──────────────────────── */
enum nf_conn_state {
    NF_CONN_NEW          = 0,
    NF_CONN_ESTABLISHED  = 1,
    NF_CONN_RELATED      = 2,
    NF_CONN_CLOSED       = 3,
};

/* ── Connection tracking entry ─────────────────────────────────────── */
#define NF_CONNTRACK_MAX   256  /* increased from 64 */

struct nf_conn {
    /* Tuple */
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;

    /* Protocol-specific state */
    uint8_t  proto_state;    /* TCP_CONN_*, UDP_CONN_*, ICMP_CONN_* */
    uint8_t  tcp_flags_seen; /* OR of TCP flags seen on this connection */
    uint16_t tcp_wscale;     /* window scale from SYN */

    /* Direction tracking */
    uint8_t  orig_saw_reply; /* saw traffic in reply direction */
    uint8_t  reply_saw_orig; /* saw traffic back from originator */

    /* Timeout management */
    uint32_t timeout_ticks;  /* current timeout value in timer ticks */
    uint64_t last_seen;      /* tick when last packet matched */
    uint8_t  timeout_idx;    /* index into per-protocol timeout table */
    uint8_t  used;

    /* Reference count for external callers (get/put API).
     * Starts at 1 for the table's implicit reference.
     * nf_conntrack_get increments; nf_conntrack_put decrements.
     * Entry is freed when refcount reaches 0.
     * nf_conntrack_purge only expires entries with refcount == 1. */
    int      refcount;

    /* Counters */
    uint64_t packets;
    uint64_t bytes;
    uint64_t packets_reply;
    uint64_t bytes_reply;

    /* Mark for userspace/iptables */
    uint32_t mark;

    /* Linkage for hash table (if needed) — just linear scan for now */
};

/* ── NAT rule ─────────────────────────────────────────────────────── */
struct nf_nat_rule {
    uint32_t orig_ip;
    uint16_t orig_port;
    uint32_t new_ip;
    uint16_t new_port;
    int      used;
};

/* ── Conntrack statistics ──────────────────────────────────────────── */
struct nf_conntrack_stats {
    uint64_t total_lookups;
    uint64_t total_creations;
    uint64_t total_destroys;
    uint64_t total_expired;
    uint64_t table_full_errors;
    uint64_t current_active;
    uint64_t max_active;
    uint64_t tcp_states[TCP_CONN_MAX_STATE];
};

/* ── Netfilter API ────────────────────────────────────────────────── */

/* Hook management */
int  nf_register_hook(int hook, nf_hookfn fn, int priority);
void nf_unregister_hook(int hook, nf_hookfn fn);
int  nf_iterate_hooks(int hook, void *skb);

/* Traverse hooks and process verdict (handles NF_DROP/NF_REJECT/ICMP).
 * Returns 0 if packet accepted, -1 if dropped/rejected. */
int  nf_hook_traverse(int hook, void *skb, void *iph, uint16_t iph_len);

/* Rule management */
int  nf_add_rule(const struct nf_rule *rule);
int  nf_del_rule(const struct nf_rule *rule);
void nf_flush_rules(void);
void nf_print_rules(void);
int  nf_check_rules(void *skb, uint32_t src_ip, uint32_t dst_ip,
                    uint16_t src_port, uint16_t dst_port, uint8_t protocol);

/* ── Conntrack API ─────────────────────────────────────────────────── */

/* Handle a packet in the conntrack system (called per-packet) */
int  nf_conntrack_in(uint32_t src_ip, uint32_t dst_ip,
                     uint16_t src_port, uint16_t dst_port,
                     uint8_t protocol, uint8_t tcp_flags,
                     uint16_t payload_len);

int  nf_conntrack_out(uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint8_t protocol, uint8_t tcp_flags,
                      uint16_t payload_len);

/* Lookup a connection entry without modifying it */
struct nf_conn *nf_conntrack_lookup(uint32_t src_ip, uint32_t dst_ip,
                                    uint16_t src_port, uint16_t dst_port,
                                    uint8_t protocol);

/* Update conntrack state based on TCP flags (TCP state machine) */
void nf_conntrack_update_tcp(struct nf_conn *conn, uint8_t tcp_flags,
                             int from_originator);

/* Get/put for reference counting (legacy API compatibility) */
struct nf_conn *nf_conntrack_get(uint32_t src_ip, uint32_t dst_ip,
                                 uint16_t src_port, uint16_t dst_port,
                                 uint8_t protocol);
void nf_conntrack_put(struct nf_conn *conn);
void nf_conntrack_timeout(struct nf_conn *conn, uint32_t timeout_ticks);

/* Periodically expire old entries — call from timer/softirq */
void nf_conntrack_purge(void);

/* Get statistics snapshot */
void nf_conntrack_stats_get(struct nf_conntrack_stats *stats);

/* Dump all tracked connections (for /proc/net/conntrack) */
int  nf_conntrack_dump(struct nf_conn *buf, int max);

/* Init */
void nf_init(void);
void nf_conntrack_init(void);

#endif /* NETFILTER_H */
