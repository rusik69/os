#ifndef NF_TABLES_H
#define NF_TABLES_H

#include "types.h"
#include "netfilter.h"

/* ── nftables rule (B5) ─────────────────────────────────────────── */

/* nftables rule with match + action */
struct nft_rule {
    /* Match critera */
    uint32_t src_ip;
    uint32_t src_mask;
    uint32_t dst_ip;
    uint32_t dst_mask;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;       /* IPPROTO_TCP/UDP/ICMP, 0 = any */
    uint8_t  in_iface;       /* input interface index (0 = any) */
    uint8_t  out_iface;      /* output interface index (0 = any) */

    /* Action */
    uint8_t  action;         /* NF_ACCEPT, NF_DROP, NF_REJECT */
    uint8_t  verdict;        /* NFT_VERDICT_* */
    uint32_t counter_packets;
    uint32_t counter_bytes;

    /* Linkage */
    struct nft_rule *next;
};

/* nftables set for IP/port matching (like nftables set) */
struct nft_set_elem {
    uint32_t ip;
    uint16_t port;
    uint8_t  protocol;
    uint8_t  used;
    uint32_t timeout_ms;     /* 0 = permanent */
};

#define NFT_SET_MAX_ELEMS 256

struct nft_set {
    char               name[32];
    uint8_t            type;    /* NFT_SET_IPV4, NFT_SET_PORT, NFT_SET_IPV4_PORT */
    struct nft_set_elem elems[NFT_SET_MAX_ELEMS];
    uint32_t           n_elems;
};

/* Set types */
#define NFT_SET_IPV4       1
#define NFT_SET_PORT       2
#define NFT_SET_IPV4_PORT  3

/* Verdict types */
#define NFT_VERDICT_ACCEPT   0
#define NFT_VERDICT_DROP     1
#define NFT_VERDICT_REJECT   2
#define NFT_VERDICT_JUMP     3
#define NFT_VERDICT_GOTO     4
#define NFT_VERDICT_RETURN   5
#define NFT_VERDICT_CONTINUE 6

/* Chain type */
#define NFT_CHAIN_FILTER   0
#define NFT_CHAIN_NAT      1
#define NFT_CHAIN_ROUTE    2

/* Chain hook type (maps to NF_INET_*) */
#define NFT_HOOK_PRE_ROUTING  0
#define NFT_HOOK_LOCAL_IN     1
#define NFT_HOOK_FORWARD      2
#define NFT_HOOK_LOCAL_OUT    3
#define NFT_HOOK_POST_ROUTING 4

#define NFT_CHAIN_MAX_RULES 128
#define NFT_MAX_CHAINS       32

struct nft_chain {
    char            name[32];
    uint8_t         type;       /* NFT_CHAIN_FILTER, etc */
    uint8_t         hook_num;   /* NF_INET_PRE_ROUTING, etc */
    int             priority;
    struct nft_rule *rules;
    uint32_t        rule_count;
    uint32_t        flags;
};

#define NFT_MAX_TABLES 8

struct nft_table {
    char              name[32];
    uint8_t           family;     /* NFPROTO_IPV4 = 2 */
    uint32_t          flags;      /* NFT_TABLE_F_* */
    struct nft_chain  chains[NFT_MAX_CHAINS];
    uint32_t          n_chains;
    struct nft_set    sets[16];
    uint32_t          n_sets;

    /* Use count / busy flag for atomic swap */
    volatile int      active;
};

/* Table flags */
#define NFT_TABLE_F_DORMANT    0x01
#define NFT_TABLE_F_ACTIVE     0x02

/* Protocol families */
#define NFPROTO_IPV4   2
#define NFPROTO_IPV6  10
#define NFPROTO_NETDEV 12

/* ── API ────────────────────────────────────────────────────────── */

/* Table management */
int  nft_register_table(struct nft_table *table);
void nft_unregister_table(struct nft_table *table);

/* Rule management within a chain */
int  nft_add_rule(struct nft_chain *chain, const struct nft_rule *rule);
int  nft_del_rule(struct nft_chain *chain, uint32_t index);
void nft_flush_rules(struct nft_chain *chain);

/* Atomic table swap — single-call update */
int  nft_apply(struct nft_table *old, struct nft_table *new);

/* Set management */
int  nft_set_add(struct nft_set *set, uint32_t ip, uint16_t port, uint8_t proto, uint32_t timeout_ms);
int  nft_set_del(struct nft_set *set, uint32_t ip, uint16_t port, uint8_t proto);
int  nft_set_lookup(struct nft_set *set, uint32_t ip, uint16_t port, uint8_t proto);

/* Packet evaluation against nftables rules */
int  nft_evaluate(struct nft_table *table, void *skb,
                  uint32_t src_ip, uint32_t dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t protocol, int hook);

/* Hook function for netfilter integration */
int  nft_hook_handler(void *skb, int hook);

/* Initialization / teardown */
int  nft_init(void);
void nft_exit(void);

#endif /* NF_TABLES_H */
