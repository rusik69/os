#ifndef NETFILTER_H
#define NETFILTER_H

#include "types.h"

/* Netfilter hook points (Linux-compatible numbering) */
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

/* Connection tracking entry */
#define NF_CONNTRACK_MAX 64

enum nf_conn_state {
    NF_CONN_NEW      = 0,
    NF_CONN_ESTABLISHED = 1,
    NF_CONN_RELATED  = 2,
    NF_CONN_CLOSED   = 3,
};

struct nf_conn {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
    uint32_t state;
    uint32_t timeout;
    int      used;
};

/* NAT rule */
struct nf_nat_rule {
    uint32_t orig_ip;
    uint16_t orig_port;
    uint32_t new_ip;
    uint16_t new_port;
    int      used;
};

/* ── Netfilter API ──────────────────────────────────────────────── */

/* Hook management */
int  nf_register_hook(int hook, nf_hookfn fn, int priority);
void nf_unregister_hook(int hook, nf_hookfn fn);
int  nf_iterate_hooks(int hook, void *skb);

/* Rule management */
int  nf_add_rule(const struct nf_rule *rule);
int  nf_del_rule(const struct nf_rule *rule);
void nf_flush_rules(void);
int  nf_check_rules(void *skb, uint32_t src_ip, uint32_t dst_ip,
                    uint16_t src_port, uint16_t dst_port, uint8_t protocol);

/* Connection tracking */
struct nf_conn *nf_conntrack_get(uint32_t src_ip, uint32_t dst_ip,
                                 uint16_t src_port, uint16_t dst_port,
                                 uint8_t protocol);
void nf_conntrack_put(struct nf_conn *conn);
void nf_conntrack_timeout(struct nf_conn *conn, uint32_t timeout);
void nf_conntrack_purge(void);

/* NAT */
int  nf_nat_register_rule(uint32_t orig_ip, uint16_t orig_port,
                          uint32_t new_ip, uint16_t new_port);
int  nf_nat_apply_pre_routing(uint32_t *ip, uint16_t *port);
int  nf_nat_apply_post_routing(uint32_t *ip, uint16_t *port);

/* Init */
void nf_init(void);

#endif /* NETFILTER_H */
