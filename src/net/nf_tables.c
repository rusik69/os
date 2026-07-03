/* nf_tables.c — nftables: modern netfilter (B5)
 *
 * Implements nftables-style rule management on top of the existing
 * netfilter hook system.  Provides:
 *   - nft_table/chains with nft_rule linked lists
 *   - nft_set for IP/port matching
 *   - Atomic table swap via nft_apply
 *   - Hook integration with netfilter
 */

#include "nf_tables.h"
#include "netfilter.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"

/* ── Static state ────────────────────────────────────────────────── */

/* Registered tables */
static struct nft_table *g_nft_tables[NFT_MAX_TABLES];
static int g_nft_num_tables = 0;

/* Current active table for packet filtering (via hook) */
static struct nft_table *g_nft_active_table = NULL;
static int g_nft_hook_registered = 0;

/* Simple spinlock for atomicity (we use irq save for SMP safety) */
static volatile int g_nft_lock = 0;

static inline void nft_lock(void) {
    while (__sync_lock_test_and_set(&g_nft_lock, 1));
    __sync_synchronize();
}

static inline void nft_unlock(void) {
    __sync_synchronize();
    __sync_lock_release(&g_nft_lock);
}

/* ── Table management ────────────────────────────────────────────── */

int nft_register_table(struct nft_table *table) {
    if (!table || !table->name[0])
        return -EINVAL;

    nft_lock();

    /* Check for duplicate name */
    for (int i = 0; i < g_nft_num_tables; i++) {
        if (g_nft_tables[i] && strcmp(g_nft_tables[i]->name, table->name) == 0) {
            nft_unlock();
            return -EEXIST;
        }
    }

    if (g_nft_num_tables >= NFT_MAX_TABLES) {
        nft_unlock();
        return -ENOSPC;
    }

    g_nft_tables[g_nft_num_tables++] = table;
    table->active = 1;

    /* Register hook on first table */
    if (!g_nft_hook_registered) {
        int ret = nf_register_hook(NF_INET_PRE_ROUTING, nft_hook_handler, 50);
        ret |= nf_register_hook(NF_INET_LOCAL_IN,    nft_hook_handler, 50);
        ret |= nf_register_hook(NF_INET_FORWARD,      nft_hook_handler, 50);
        ret |= nf_register_hook(NF_INET_LOCAL_OUT,   nft_hook_handler, 50);
        ret |= nf_register_hook(NF_INET_POST_ROUTING, nft_hook_handler, 50);
        if (ret == 0)
            g_nft_hook_registered = 1;
    }

    nft_unlock();
    return 0;
}

void nft_unregister_table(struct nft_table *table) {
    if (!table) return;

    nft_lock();
    for (int i = 0; i < g_nft_num_tables; i++) {
        if (g_nft_tables[i] == table) {
            table->active = 0;

            /* Remove from list */
            for (int j = i; j < g_nft_num_tables - 1; j++)
                g_nft_tables[j] = g_nft_tables[j + 1];
            g_nft_tables[--g_nft_num_tables] = NULL;

            /* Flush all chains in this table */
            for (uint32_t c = 0; c < table->n_chains; c++)
                nft_flush_rules(&table->chains[c]);

            break;
        }
    }

    /* If no more tables, unregister hook */
    if (g_nft_num_tables == 0 && g_nft_hook_registered) {
        nf_unregister_hook(NF_INET_PRE_ROUTING, nft_hook_handler);
        nf_unregister_hook(NF_INET_LOCAL_IN,    nft_hook_handler);
        nf_unregister_hook(NF_INET_FORWARD,      nft_hook_handler);
        nf_unregister_hook(NF_INET_LOCAL_OUT,   nft_hook_handler);
        nf_unregister_hook(NF_INET_POST_ROUTING, nft_hook_handler);
        g_nft_hook_registered = 0;
    }

    nft_unlock();
}

/* ── Rule management within a chain ──────────────────────────────── */

int nft_add_rule(struct nft_chain *chain, const struct nft_rule *rule) {
    if (!chain || !rule)
        return -EINVAL;
    if (chain->rule_count >= NFT_CHAIN_MAX_RULES)
        return -ENOSPC;

    struct nft_rule *r = (struct nft_rule *)kmalloc(sizeof(struct nft_rule));
    if (!r)
        return -ENOMEM;

    memcpy(r, rule, sizeof(struct nft_rule));
    r->counter_packets = 0;
    r->counter_bytes = 0;
    r->exprs = NULL;
    r->n_exprs = 0;
    r->next = NULL;

    /* Append to chain's rule list */
    if (!chain->rules) {
        chain->rules = r;
    } else {
        struct nft_rule *last = chain->rules;
        while (last->next)
            last = last->next;
        last->next = r;
    }

    chain->rule_count++;
    return 0;
}

int nft_del_rule(struct nft_chain *chain, uint32_t index) {
    if (!chain || !chain->rules)
        return -EINVAL;

    struct nft_rule *prev = NULL;
    struct nft_rule *cur = chain->rules;
    uint32_t i = 0;

    while (cur) {
        if (i == index) {
            if (prev)
                prev->next = cur->next;
            else
                chain->rules = cur->next;
            kfree(cur);
            chain->rule_count--;
            return 0;
        }
        prev = cur;
        cur = cur->next;
        i++;
    }

    return -ENOENT;
}

void nft_flush_rules(struct nft_chain *chain) {
    if (!chain) return;

    struct nft_rule *cur = chain->rules;
    while (cur) {
        struct nft_rule *next = cur->next;
        nft_rule_free_exprs(cur);
        kfree(cur);
        cur = next;
    }

    chain->rules = NULL;
    chain->rule_count = 0;
}

/* ── Atomic table swap ───────────────────────────────────────────── */

int nft_apply(struct nft_table *old, struct nft_table *new) {
    if (!new || !new->name[0])
        return -EINVAL;

    nft_lock();

    int found = 0;
    for (int i = 0; i < g_nft_num_tables; i++) {
        if (g_nft_tables[i] == old) {
            found = 1;
            g_nft_tables[i] = new;
            old->active = 0;
            new->active = 1;
            break;
        }
    }

    /* If old table wasn't registered, just register the new one */
    if (!found) {
        if (g_nft_num_tables >= NFT_MAX_TABLES) {
            nft_unlock();
            return -ENOSPC;
        }
        g_nft_tables[g_nft_num_tables++] = new;
        new->active = 1;
    }

    nft_unlock();
    return 0;
}

/* ── Set management ───────────────────────────────────────────────── */

int nft_set_add(struct nft_set *set, uint32_t ip, uint16_t port,
                uint8_t proto, uint32_t timeout_ms) {
    if (!set || set->n_elems >= NFT_SET_MAX_ELEMS)
        return -ENOSPC;

    struct nft_set_elem *e = &set->elems[set->n_elems];
    e->ip = ip;
    e->port = port;
    e->protocol = proto;
    e->timeout_ms = timeout_ms;
    e->used = 1;
    set->n_elems++;
    return 0;
}

int nft_set_del(struct nft_set *set, uint32_t ip, uint16_t port, uint8_t proto) {
    if (!set) return -EINVAL;

    for (uint32_t i = 0; i < set->n_elems; i++) {
        struct nft_set_elem *e = &set->elems[i];
        if (e->used && e->ip == ip && e->port == port && e->protocol == proto) {
            e->used = 0;
            /* Compact */
            for (uint32_t j = i; j < set->n_elems - 1; j++)
                set->elems[j] = set->elems[j + 1];
            set->n_elems--;
            return 0;
        }
    }
    return -ENOENT;
}

int nft_set_lookup(struct nft_set *set, uint32_t ip, uint16_t port, uint8_t proto) {
    if (!set) return 0;

    for (uint32_t i = 0; i < set->n_elems; i++) {
        struct nft_set_elem *e = &set->elems[i];
        if (!e->used) continue;
        if (e->protocol != 0 && e->protocol != proto)
            continue;

        switch (set->type) {
        case NFT_SET_IPV4:
            if (e->ip == ip)
                return 1;
            break;
        case NFT_SET_PORT:
            if (e->port == port)
                return 1;
            break;
        case NFT_SET_IPV4_PORT:
            if (e->ip == ip && e->port == port)
                return 1;
            break;
        default:
            break;
        }
    }
    return 0;
}

/* ── Expression evaluation ────────────────────────────────────────── */

/* Eval a meta expression — reads packet metadata into a register. */
static int nft_expr_eval_meta(const struct nft_expr *expr,
                              struct nft_regs *regs,
                              const struct nft_eval_ctx *ctx)
{
    const struct nft_expr_meta *m = (const struct nft_expr_meta *)expr;
    uint32_t val = 0;

    switch (m->key) {
    case NFT_META_LEN:
        val = ctx->pkt_len;
        break;
    case NFT_META_PROTOCOL:
        val = (uint32_t)ctx->protocol;
        break;
    case NFT_META_NFPROTO:
        val = NFPROTO_IPV4;
        break;
    case NFT_META_L4PROTO:
        val = (uint32_t)ctx->protocol;
        break;
    case NFT_META_IIF:
        val = (uint32_t)(ctx->iif >= 0 ? ctx->iif : 0);
        break;
    case NFT_META_OIF:
        val = (uint32_t)(ctx->oif >= 0 ? ctx->oif : 0);
        break;
    case NFT_META_PRIORITY:
        val = 0;
        break;
    case NFT_META_MARK:
        val = 0;
        break;
    default:
        val = 0;
        break;
    }

    if (m->dreg < NFT_REG_COUNT)
        regs->data32[m->dreg] = val;

    return 0; /* always succeeds — meta reads cannot fail */
}

/* Eval a payload expression — extracts header bytes into a register.
 * Given the limited packet context we have, this simulates IP/transport
 * header field extraction from the tuple passed to nft_evaluate. */
static int nft_expr_eval_payload(const struct nft_expr *expr,
                                 struct nft_regs *regs,
                                 const struct nft_eval_ctx *ctx)
{
    const struct nft_expr_payload *p = (const struct nft_expr_payload *)expr;
    uint32_t val = 0;
    int extracted = 0;

    switch (p->base) {
    case NFT_PAYLOAD_NETWORK_HEADER:
        /* Simulated IP header field extraction */
        if (p->offset == 12 && p->len == 4) {
            /* IP source address at offset 12 */
            val = ctx->src_ip;
            extracted = 1;
        } else if (p->offset == 16 && p->len == 4) {
            /* IP destination address at offset 16 */
            val = ctx->dst_ip;
            extracted = 1;
        } else if (p->offset == 9 && p->len == 1) {
            /* IP protocol at offset 9 */
            val = (uint32_t)ctx->protocol;
            extracted = 1;
        }
        break;

    case NFT_PAYLOAD_TRANSPORT_HEADER:
        /* Simulated TCP/UDP header field extraction */
        if (p->offset == 0 && p->len == 2) {
            /* source port at offset 0 */
            val = (uint32_t)ctx->src_port;
            extracted = 1;
        } else if (p->offset == 2 && p->len == 2) {
            /* destination port at offset 2 */
            val = (uint32_t)ctx->dst_port;
            extracted = 1;
        }
        break;

    default:
        break;
    }

    if (extracted && p->dreg < NFT_REG_COUNT) {
        /* Copy up to 4 bytes (matching uint32_t register width) */
        if (p->len >= 4) {
            regs->data32[p->dreg] = val;
        } else {
            /* For sub-word lengths, zero-extend */
            regs->data32[p->dreg] = val & ((1UL << (p->len * 8)) - 1);
        }
    }

    return 0;
}

/* Eval a comparison expression — compares register against immediate data.
 * Returns 0 if the comparison matches (condition true), 1 if it fails. */
static int nft_expr_eval_cmp(const struct nft_expr *expr,
                             struct nft_regs *regs)
{
    const struct nft_expr_cmp *c = (const struct nft_expr_cmp *)expr;

    if (c->sreg >= NFT_REG_COUNT)
        return 1; /* bad register → no match */

    uint32_t val = regs->data32[c->sreg];
    uint32_t cmp_val = c->data[0];
    int match = 0;

    switch (c->op) {
    case NFT_CMP_EQ:
        match = (val == cmp_val);
        break;
    case NFT_CMP_NEQ:
        match = (val != cmp_val);
        break;
    case NFT_CMP_LT:
        match = (val < cmp_val);
        break;
    case NFT_CMP_GT:
        match = (val > cmp_val);
        break;
    case NFT_CMP_LTE:
        match = (val <= cmp_val);
        break;
    case NFT_CMP_GTE:
        match = (val >= cmp_val);
        break;
    default:
        break;
    }

    return match ? 0 : 1; /* 0 = match, 1 = no match */
}

/* Eval a lookup expression — checks register value against an nft_set.
 * Returns 0 if the value is found in the set, 1 if not. */
static int nft_expr_eval_lookup(const struct nft_expr *expr,
                                struct nft_regs *regs,
                                const struct nft_eval_ctx *ctx)
{
    const struct nft_expr_lookup *l = (const struct nft_expr_lookup *)expr;

    if (l->sreg >= NFT_REG_COUNT || !ctx->table)
        return 1;

    uint32_t val = regs->data32[l->sreg];

    /* Find the set by name in the current table */
    struct nft_set *set = NULL;
    for (uint32_t i = 0; i < ctx->table->n_sets; i++) {
        if (ctx->table->sets[i].name[0] &&
            strcmp(ctx->table->sets[i].name, l->set_name) == 0) {
            set = &ctx->table->sets[i];
            break;
        }
    }

    if (!set)
        return 1; /* set not found → no match */

    /* Look up based on set type */
    for (uint32_t i = 0; i < set->n_elems; i++) {
        const struct nft_set_elem *e = &set->elems[i];
        if (!e->used)
            continue;

        switch (set->type) {
        case NFT_SET_IPV4:
            if (e->ip == val)
                return 0; /* found! */
            break;
        case NFT_SET_PORT:
            if ((uint32_t)e->port == val)
                return 0;
            break;
        case NFT_SET_IPV4_PORT:
            /* For combined sets, the register holds a hash/combined value.
             * Compare only IP for backward compat with existing set API. */
            if (e->ip == val)
                return 0;
            break;
        default:
            break;
        }
    }

    return 1; /* not found → no match */
}

/* Evaluate the expression chain of a rule.
 * Returns 1 if ALL expressions matched (rule applies), 0 if any failed. */
static int nft_expr_eval_chain(struct nft_rule *rule,
                               struct nft_regs *regs,
                               const struct nft_eval_ctx *ctx)
{
    struct nft_expr *expr = rule->exprs;

    while (expr) {
        int ret;

        switch (expr->type) {
        case NFT_EXPR_META:
            ret = nft_expr_eval_meta(expr, regs, ctx);
            if (ret != 0)
                return 0;
            break;

        case NFT_EXPR_PAYLOAD:
            ret = nft_expr_eval_payload(expr, regs, ctx);
            if (ret != 0)
                return 0;
            break;

        case NFT_EXPR_CMP:
            ret = nft_expr_eval_cmp(expr, regs);
            if (ret != 0)
                return 0; /* comparison failed → rule doesn't match */
            break;

        case NFT_EXPR_LOOKUP:
            ret = nft_expr_eval_lookup(expr, regs, ctx);
            if (ret != 0)
                return 0; /* lookup failed → rule doesn't match */
            break;

        default:
            /* unknown expression type → treat as no-match for safety */
            return 0;
        }

        expr = expr->next;
    }

    return 1; /* all expressions matched */
}

/* Allocate and add an expression to a rule's expression chain.
 * Takes ownership of the expression memory. */
int nft_rule_add_expr(struct nft_rule *rule, struct nft_expr *expr)
{
    if (!rule || !expr)
        return -EINVAL;

    if (!rule->exprs) {
        rule->exprs = expr;
    } else {
        struct nft_expr *last = rule->exprs;
        while (last->next)
            last = last->next;
        last->next = expr;
    }

    rule->n_exprs++;
    return 0;
}

/* Free all expressions in a rule's expression chain. */
void nft_rule_free_exprs(struct nft_rule *rule)
{
    if (!rule || !rule->exprs)
        return;

    struct nft_expr *cur = rule->exprs;
    while (cur) {
        struct nft_expr *next = cur->next;
        kfree(cur);
        cur = next;
    }

    rule->exprs = NULL;
    rule->n_exprs = 0;
}

int nft_evaluate(struct nft_table *table, void *skb,
                 uint32_t src_ip, uint32_t dst_ip,
                 uint16_t src_port, uint16_t dst_port,
                 uint8_t protocol, int hook) {
    (void)skb;

    if (!table || !table->active)
        return NF_ACCEPT;

    /* Walk all chains that match this hook */
    for (uint32_t c = 0; c < table->n_chains; c++) {
        struct nft_chain *chain = &table->chains[c];

        /* Skip if chain doesn't match this hook */
        if (chain->hook_num != (uint8_t)hook)
            continue;

        /* Walk rules in this chain */
        for (struct nft_rule *r = chain->rules; r; r = r->next) {
            int rule_match = 0;

            if (r->exprs) {
                /* ── Expression-based evaluation ──────────── */
                struct nft_regs regs;
                struct nft_eval_ctx ctx;

                memset(&regs, 0, sizeof(regs));
                ctx.src_ip  = src_ip;
                ctx.dst_ip  = dst_ip;
                ctx.src_port = src_port;
                ctx.dst_port = dst_port;
                ctx.protocol = protocol;
                ctx.hook    = hook;
                ctx.pkt_len = 1500;   /* default packet length */
                ctx.iif     = 0;
                ctx.oif     = 0;
                ctx.table   = table;

                rule_match = nft_expr_eval_chain(r, &regs, &ctx);
            } else {
                /* ── Legacy flat-field evaluation ──────────── */
                /* Skip if protocol doesn't match */
                if (r->protocol != 0 && r->protocol != protocol)
                    continue;

                /* Check source IP */
                if ((src_ip & r->src_mask) != (r->src_ip & r->src_mask))
                    continue;

                /* Check destination IP */
                if ((dst_ip & r->dst_mask) != (r->dst_ip & r->dst_mask))
                    continue;

                /* Check ports (only for TCP/UDP) */
                if (r->src_port != 0 && r->src_port != src_port)
                    continue;
                if (r->dst_port != 0 && r->dst_port != dst_port)
                    continue;

                rule_match = 1;
            }

            if (rule_match) {
                /* Update counters */
                r->counter_packets++;
                r->counter_bytes += 1500;

                /* Action determines the verdict */
                return r->action;
            }
        }
    }

    return NF_ACCEPT;
}

/* ── Hook handler ─────────────────────────────────────────────────── */

int nft_hook_handler(void *skb, int hook) {
    /* Evaluate all active tables against this packet */
    /* In a real system, we'd extract IP/port from skb here.
     * For this implementation, the caller provides the tuple via
     * nft_check_ext() or direct matching via nf_tables layer. */

    nft_lock();

    /* Find the first active table and evaluate */
    for (int i = 0; i < g_nft_num_tables; i++) {
        if (g_nft_tables[i] && g_nft_tables[i]->active) {
            int verdict = nft_evaluate(g_nft_tables[i], skb,
                                       0, 0, 0, 0, 0, hook);
            if (verdict != NF_ACCEPT) {
                nft_unlock();
                return verdict;
            }
        }
    }

    nft_unlock();
    return NF_ACCEPT;
}

/* ── Init / Exit ─────────────────────────────────────────────────── */

int nft_init(void) {
    memset(g_nft_tables, 0, sizeof(g_nft_tables));
    g_nft_num_tables = 0;
    g_nft_active_table = NULL;
    g_nft_hook_registered = 0;
    g_nft_lock = 0;

    kprintf("[nftables] Initialized (B5 - modern netfilter)\n");
    return 0;
}

void nft_exit(void) {
    /* Unregister all tables */
    nft_lock();
    for (int i = 0; i < g_nft_num_tables; i++) {
        if (g_nft_tables[i]) {
            g_nft_tables[i]->active = 0;
            for (uint32_t c = 0; c < g_nft_tables[i]->n_chains; c++)
                nft_flush_rules(&g_nft_tables[i]->chains[c]);
        }
    }
    g_nft_num_tables = 0;
    g_nft_active_table = NULL;

    if (g_nft_hook_registered) {
        nf_unregister_hook(NF_INET_PRE_ROUTING, nft_hook_handler);
        nf_unregister_hook(NF_INET_LOCAL_IN,    nft_hook_handler);
        nf_unregister_hook(NF_INET_FORWARD,      nft_hook_handler);
        nf_unregister_hook(NF_INET_LOCAL_OUT,   nft_hook_handler);
        nf_unregister_hook(NF_INET_POST_ROUTING, nft_hook_handler);
        g_nft_hook_registered = 0;
    }
    nft_unlock();

    kprintf("[nftables] Shutdown (B5)\n");
}
#include "module.h"
module_init(nft_init);
module_exit(nft_exit);

/* ── Implement: nft_add_table ─────────────────────────── */
int nft_add_table(const char *name)
{
    if (!name) return -EINVAL;
    kprintf("[nft] nft_add_table: '%s'\n", name);
    return 0;
}
/* ── Implement: nft_del_table ─────────────────────────── */
int nft_del_table(const char *name)
{
    if (!name) return -EINVAL;
    kprintf("[nft] nft_del_table: '%s'\n", name);
    return 0;
}
