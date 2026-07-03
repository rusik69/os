#ifndef NF_TABLES_H
#define NF_TABLES_H

#include "types.h"
#include "netfilter.h"
#include "hashtable.h"

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

    /* Target chain name for JUMP/GOTO verdicts */
    char     target_chain[32];

    /* Optional expression chain — when non-NULL, overrides the flat
     * matching fields above (src_ip, src_mask, etc.) with composable
     * expression-based evaluation.  Allocated with kmalloc per rule. */
    struct nft_expr *exprs;
    uint32_t        n_exprs;

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

/* RB-tree set node for range-based matching */
struct nft_rb_node {
    struct nft_rb_node *left;
    struct nft_rb_node *right;
    struct nft_rb_node *parent;
    uint32_t            ip_min;
    uint32_t            ip_max;
    uint16_t            port_min;
    uint16_t            port_max;
    uint8_t             protocol;
    uint8_t             color;    /* 0=red, 1=black */
};

#define NFT_SET_MAX_ELEMS 256

struct nft_set {
    char               name[32];
    uint8_t            type;       /* NFT_SET_IPV4, NFT_SET_PORT, NFT_SET_IPV4_PORT */
    uint8_t            backend;    /* NFT_SET_BACKEND_* */
    uint8_t            pad[2];

    /* Array backend (original, default) */
    struct nft_set_elem elems[NFT_SET_MAX_ELEMS];
    uint32_t           n_elems;

    /* Backend-specific data */
    union {
        struct hashtable hash;              /* NFT_SET_BACKEND_HASH */
        struct {                             /* NFT_SET_BACKEND_RBTREE */
            struct nft_rb_node *root;
            int                count;
        } rb;
        struct {                             /* NFT_SET_BACKEND_BITMAP */
            unsigned long    *bitmap;        /* dynamically allocated */
            int               nbits;         /* number of bits */
        } bmp;
    } data;
};

/* Set types */
#define NFT_SET_IPV4       1
#define NFT_SET_PORT       2
#define NFT_SET_IPV4_PORT  3

/* Set backend types */
#define NFT_SET_BACKEND_ARRAY   0  /* flat array (default) */
#define NFT_SET_BACKEND_HASH    1  /* hash table backed */
#define NFT_SET_BACKEND_RBTREE  2  /* red-black tree for ranges */
#define NFT_SET_BACKEND_BITMAP  3  /* bitmap for port matching */

/* Verdict types */
#define NFT_VERDICT_ACCEPT   0
#define NFT_VERDICT_DROP     1
#define NFT_VERDICT_REJECT   2
#define NFT_VERDICT_JUMP     3
#define NFT_VERDICT_GOTO     4
#define NFT_VERDICT_RETURN   5
#define NFT_VERDICT_CONTINUE 6
#define NFT_VERDICT_QUEUE    7

/* Jump stack recursion limit (max nested chain jumps) */
#define NFT_JUMP_STACK_DEPTH 8

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
#define NFT_TABLE_F_ACTIVE    0x02

/* ── Expression System ───────────────────────────────────────────── */

/* Register numbers for expression evaluation */
#define NFT_REG_VERDICT  0
#define NFT_REG_1        1
#define NFT_REG_2        2
#define NFT_REG_3        3
#define NFT_REG_COUNT    4

/* Expression types */
#define NFT_EXPR_META      0
#define NFT_EXPR_PAYLOAD   1
#define NFT_EXPR_CMP       2
#define NFT_EXPR_LOOKUP    3
#define NFT_EXPR_IMMEDIATE 4
#define NFT_EXPR_COUNTER   5
#define NFT_EXPR_NAT       6
#define NFT_EXPR_LOG       7
#define NFT_EXPR_LIMIT     8

/* NAT types for NFT_EXPR_NAT */
#define NFT_NAT_DNAT       0   /* Destination NAT (pre-routing) */
#define NFT_NAT_SNAT       1   /* Source NAT (post-routing) */
#define NFT_NAT_MASQUERADE 2   /* Masquerade (SNAT with dynamic address) */

/* Meta keys for NFT_EXPR_META */
#define NFT_META_LEN       0   /* packet length */
#define NFT_META_PROTOCOL  1   /* ethertype/L3 protocol */
#define NFT_META_NFPROTO   2   /* netfilter protocol family */
#define NFT_META_L4PROTO   3   /* L4 protocol number */
#define NFT_META_IIF       4   /* input interface index */
#define NFT_META_OIF       5   /* output interface index */
#define NFT_META_PRIORITY  6   /* packet priority */
#define NFT_META_MARK      7   /* packet mark */

/* Payload bases for NFT_EXPR_PAYLOAD */
#define NFT_PAYLOAD_NETWORK_HEADER    0
#define NFT_PAYLOAD_TRANSPORT_HEADER  1

/* Comparison operators for NFT_EXPR_CMP */
#define NFT_CMP_EQ  0
#define NFT_CMP_NEQ 1
#define NFT_CMP_LT  2
#define NFT_CMP_GT  3
#define NFT_CMP_LTE 4
#define NFT_CMP_GTE 5

/* ── Expression structures ───────────────────────────────────────── */

/* Generic expression header — every expression embeds this at offset 0.
 * The type field determines which concrete struct follows, and next
 * chains expressions within a single rule. */
struct nft_expr {
    uint8_t            type;       /* NFT_EXPR_* */
    uint8_t            pad[3];     /* alignment padding */
    struct nft_expr   *next;       /* next expression in chain */
} __attribute__((packed));

/* Meta expression — reads packet metadata into a register.
 * Used to match on packet properties that aren't header fields:
 * length, interface, protocol family, etc. */
struct nft_expr_meta {
    struct nft_expr    hdr;        /* common header */
    uint8_t            key;        /* NFT_META_* */
    uint8_t            dreg;       /* destination register */
    uint8_t            pad[2];
} __attribute__((packed));

/* Payload expression — extracts header bytes into a register.
 * Reads from the network or transport header at a given offset/len. */
struct nft_expr_payload {
    struct nft_expr    hdr;        /* common header */
    uint8_t            base;       /* NFT_PAYLOAD_*_HEADER */
    uint8_t            offset;     /* byte offset within header */
    uint8_t            len;        /* number of bytes (1-4) */
    uint8_t            dreg;       /* destination register */
} __attribute__((packed));

/* Comparison expression — compares register value against immediate data.
 * If the comparison fails, the entire rule is skipped (no match). */
struct nft_expr_cmp {
    struct nft_expr    hdr;        /* common header */
    uint8_t            op;         /* NFT_CMP_* */
    uint8_t            sreg;       /* source register */
    uint8_t            len;        /* data length (1-16) */
    uint8_t            pad;
    uint32_t           data[4];    /* inline comparison data (up to 16B) */
} __attribute__((packed));

/* Lookup expression — checks register value against an nft_set.
 * The set is identified by name within the same table. */
struct nft_expr_lookup {
    struct nft_expr    hdr;        /* common header */
    char               set_name[32]; /* name of set to look up in */
    uint8_t            sreg;       /* source register */
    uint8_t            pad[3];
} __attribute__((packed));

/* Immediate value expression — stores a constant value into a register.
 * Used to load literal values (IP addresses, port numbers, marks, etc.)
 * into a register for use by subsequent expressions like CMP or LOOKUP. */
struct nft_expr_immediate {
    struct nft_expr    hdr;        /* common header */
    uint8_t            dreg;       /* destination register */
    uint8_t            len;        /* data length (1-16 bytes) */
    uint8_t            pad[2];
    uint32_t           data[4];    /* immediate value (up to 16 bytes) */
} __attribute__((packed));

/* Counter expression — counts packets and bytes passing through.
 * Increments both counters on each evaluation.
 * Always returns 0 (doesn't affect matching verdict). */
struct nft_expr_counter {
    struct nft_expr    hdr;        /* common header */
    uint32_t           packets;    /* packet counter */
    uint32_t           bytes;      /* byte counter (accumulated) */
} __attribute__((packed));

/* NAT expression — performs DNAT, SNAT, or Masquerade translation.
 * Modifies the source or destination address/port of a packet.
 * Typically used in nat-type chains:
 *   - DNAT:      PREROUTING / LOCAL_OUT chains
 *   - SNAT:      POSTROUTING / LOCAL_IN chains
 *   - MASQUERADE: POSTROUTING chain (uses outgoing interface address) */
struct nft_expr_nat {
    struct nft_expr    hdr;        /* common header */
    uint8_t            nat_type;   /* NFT_NAT_DNAT, NFT_NAT_SNAT, NFT_NAT_MASQUERADE */
    uint8_t            pad[3];
    uint32_t           addr;       /* new address (0 = not specified / use iface addr) */
    uint16_t           port_min;   /* port range start (0 = no port change) */
    uint16_t           port_max;   /* port range end */
} __attribute__((packed));

/* Log expression — logs matching packets to kernel log.
 * Always returns 0 (doesn't affect matching verdict). */
struct nft_expr_log {
    struct nft_expr    hdr;        /* common header */
    char               prefix[32]; /* log prefix string */
    uint8_t            level;      /* log level (0 = info) */
    uint8_t            flags;      /* log flags */
    uint8_t            group;      /* nflog group number */
    uint8_t            snaplen;    /* snapshot length */
} __attribute__((packed));

/* Limit expression — rate-limits matching packets.
 * Returns 0 if within rate limit, 1 if over (no match).
 * Uses token bucket algorithm for average rate + burst control. */
struct nft_expr_limit {
    struct nft_expr    hdr;        /* common header */
    uint64_t           rate;       /* average rate (packets or bytes per second) */
    uint64_t           burst;      /* burst size */
    uint32_t           tokens;     /* current token count */
    uint64_t           last_seen;  /* last timestamp in ticks */
    uint8_t            type;       /* 0 = packets/sec, 1 = bytes/sec */
    uint8_t            invert;     /* 1 = invert matching */
    uint8_t            pad[6];
} __attribute__((packed));

/* ── Expression evaluation context ───────────────────────────────── */
/* Carries per-packet state through expression evaluation.  Filled in
 * by nft_evaluate and passed to each expression's eval function. */
struct nft_eval_ctx {
    uint32_t           src_ip;
    uint32_t           dst_ip;
    uint16_t           src_port;
    uint16_t           dst_port;
    uint8_t            protocol;
    int                hook;
    uint32_t           pkt_len;
    int                iif;        /* input interface index */
    int                oif;        /* output interface index */
    struct nft_table  *table;      /* current table for set lookups */
};

/* NAT result communicated from expression evaluation to caller.
 * When a NAT expression fires, the type and translation values are
 * stored here for the caller to apply (modify skb address/port). */
struct nft_nat_result {
    uint8_t            type;       /* NFT_NAT_DNAT/SNAT/MASQUERADE */
    uint8_t            active;     /* 1 if NAT action is pending */
    uint8_t            pad[2];
    uint32_t           addr;       /* new address */
    uint16_t           port;       /* new port (0 = no port change) */
} __attribute__((packed));

/* Register file for expression evaluation — scratch storage that
 * expressions read from and write to during rule evaluation. */
struct nft_regs {
    uint32_t           data32[NFT_REG_COUNT];  /* general-purpose regs */
    int                verdict;                /* last verdict */
    struct nft_nat_result nat;                  /* NAT result from NAT expression */
};

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

/* Expression management within a rule */
int  nft_rule_add_expr(struct nft_rule *rule, struct nft_expr *expr);
void nft_rule_free_exprs(struct nft_rule *rule);

/* Atomic table swap — single-call update */
int  nft_apply(struct nft_table *old, struct nft_table *new);

/* Set management */
int  nft_set_add(struct nft_set *set, uint32_t ip, uint16_t port, uint8_t proto, uint32_t timeout_ms);
int  nft_set_del(struct nft_set *set, uint32_t ip, uint16_t port, uint8_t proto);
int  nft_set_lookup(struct nft_set *set, uint32_t ip, uint16_t port, uint8_t proto);
int  nft_set_init(struct nft_set *set, uint8_t type, uint8_t backend, const char *name);
void nft_set_destroy(struct nft_set *set);

/* Packet evaluation against nftables rules */
int  nft_evaluate(struct nft_table *table, void *skb,
                  uint32_t src_ip, uint32_t dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t protocol, int hook);

/* Verdict processing */
int  nft_verdict_apply(struct nft_rule *rule,
                       struct nft_table *table, void *skb,
                       uint32_t src_ip, uint32_t dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       uint8_t protocol, int hook);

/* Hook function for netfilter integration */
int  nft_hook_handler(void *skb, int hook);

/* Initialization / teardown */
int  nft_init(void);
void nft_exit(void);

#endif /* NF_TABLES_H */
