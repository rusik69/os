/* netfilter.c — Packet filtering framework, connection tracking, NAT */

#define KERNEL_INTERNAL
#include "netfilter.h"
#include "conntrack_helper.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "heap.h"

/* ── Static state ────────────────────────────────────────────────── */

/* Hook chains — one linked list per hook point */
static struct nf_hook_entry *nf_hooks[NF_MAX_HOOKS];

/* Packet filter rules */
#define NF_RULES_MAX 64
static struct nf_rule nf_rules[NF_RULES_MAX];
static int nf_num_rules = 0;

/* NAT rules */
#define NF_NAT_RULES_MAX 16
static struct nf_nat_rule nf_nat_rules[NF_NAT_RULES_MAX];
static int nf_nat_num_rules = 0;

/* ── Hook management ────────────────────────────────────────────── */

int nf_register_hook(int hook, nf_hookfn fn, int priority) {
    if (hook < 0 || hook >= NF_MAX_HOOKS) return -1;
    if (!fn) return -1;

    struct nf_hook_entry *entry = (struct nf_hook_entry *)
        kmalloc(sizeof(struct nf_hook_entry));
    if (!entry) return -1;

    entry->fn = fn;
    entry->priority = priority;
    entry->next = NULL;

    /* Insert in priority order (higher priority first) */
    if (!nf_hooks[hook] || nf_hooks[hook]->priority > priority) {
        entry->next = nf_hooks[hook];
        nf_hooks[hook] = entry;
    } else {
        struct nf_hook_entry *cur = nf_hooks[hook];
        while (cur->next && cur->next->priority <= priority)
            cur = cur->next;
        entry->next = cur->next;
        cur->next = entry;
    }
    return 0;
}

void nf_unregister_hook(int hook, nf_hookfn fn) {
    if (hook < 0 || hook >= NF_MAX_HOOKS || !fn) return;

    struct nf_hook_entry **pp = &nf_hooks[hook];
    while (*pp) {
        if ((*pp)->fn == fn) {
            struct nf_hook_entry *tmp = *pp;
            *pp = (*pp)->next;
            kfree(tmp);
            return;
        }
        pp = &(*pp)->next;
    }
}

int nf_iterate_hooks(int hook, void *skb) {
    if (hook < 0 || hook >= NF_MAX_HOOKS) return NF_ACCEPT;

    struct nf_hook_entry *entry = nf_hooks[hook];
    while (entry) {
        int verdict = entry->fn(skb, hook);
        if (verdict != NF_ACCEPT)
            return verdict;
        entry = entry->next;
    }
    return NF_ACCEPT;
}

/* ── Rule management ────────────────────────────────────────────── */

int nf_add_rule(const struct nf_rule *rule) {
    if (!rule) return -1;
    if (nf_num_rules >= NF_RULES_MAX) return -1;
    nf_rules[nf_num_rules++] = *rule;
    return 0;
}

int nf_del_rule(const struct nf_rule *rule) {
    if (!rule) return -1;
    for (int i = 0; i < nf_num_rules; i++) {
        if (nf_rules[i].src_ip  == rule->src_ip  &&
            nf_rules[i].src_mask == rule->src_mask &&
            nf_rules[i].dst_ip  == rule->dst_ip  &&
            nf_rules[i].dst_mask == rule->dst_mask &&
            nf_rules[i].src_port == rule->src_port &&
            nf_rules[i].dst_port == rule->dst_port &&
            nf_rules[i].protocol  == rule->protocol &&
            nf_rules[i].action   == rule->action) {
            /* Remove by shifting */
            for (int j = i; j < nf_num_rules - 1; j++)
                nf_rules[j] = nf_rules[j + 1];
            nf_num_rules--;
            return 0;
        }
    }
    return -1;
}

void nf_flush_rules(void) {
    nf_num_rules = 0;
}

/* Print all netfilter rules (for nft list ruleset) */
void nf_print_rules(void) {
    for (int i = 0; i < nf_num_rules; i++) {
        struct nf_rule *r = &nf_rules[i];
        kprintf("    chain forward {\n");
        kprintf("      rule %d: src=%08x/%08x dst=%08x/%08x ",
                i, r->src_ip, r->src_mask, r->dst_ip, r->dst_mask);
        if (r->protocol)
            kprintf("proto=%d ", r->protocol);
        if (r->src_port)
            kprintf("sport=%d ", r->src_port);
        if (r->dst_port)
            kprintf("dport=%d ", r->dst_port);
        kprintf("action=%s\n", r->action == NF_DROP ? "drop" : "accept");
        kprintf("    }\n");
    }
    if (nf_num_rules == 0) {
        kprintf("    (no rules defined)\n");
    }
}

int nf_check_rules(void *skb, uint32_t src_ip, uint32_t dst_ip,
                   uint16_t src_port, uint16_t dst_port, uint8_t protocol) {
    (void)skb;
    for (int i = 0; i < nf_num_rules; i++) {
        struct nf_rule *r = &nf_rules[i];
        /* Check protocol match (0 = any) */
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
        /* Match found */
        return r->action;
    }
    return NF_ACCEPT;  /* default: accept */
}

/* ── NAT ────────────────────────────────────────────────────────── */

int nf_nat_register_rule(uint32_t orig_ip, uint16_t orig_port,
                          uint32_t new_ip, uint16_t new_port) {
    if (nf_nat_num_rules >= NF_NAT_RULES_MAX) return -1;
    nf_nat_rules[nf_nat_num_rules].orig_ip   = orig_ip;
    nf_nat_rules[nf_nat_num_rules].orig_port = orig_port;
    nf_nat_rules[nf_nat_num_rules].new_ip    = new_ip;
    nf_nat_rules[nf_nat_num_rules].new_port  = new_port;
    nf_nat_rules[nf_nat_num_rules].used      = 1;
    nf_nat_num_rules++;
    return 0;
}

int nf_nat_apply_pre_routing(uint32_t *ip, uint16_t *port) {
    if (!ip || !port) return 0;
    for (int i = 0; i < NF_NAT_RULES_MAX; i++) {
        if (!nf_nat_rules[i].used) continue;
        /* Match on destination IP/port (DNAT) */
        if (nf_nat_rules[i].orig_ip == *ip &&
            (nf_nat_rules[i].orig_port == 0 || nf_nat_rules[i].orig_port == *port)) {
            *ip   = nf_nat_rules[i].new_ip;
            *port = nf_nat_rules[i].new_port;
            return 1;
        }
    }
    return 0;
}

int nf_nat_apply_post_routing(uint32_t *ip, uint16_t *port) {
    if (!ip || !port) return 0;
    for (int i = 0; i < NF_NAT_RULES_MAX; i++) {
        if (!nf_nat_rules[i].used) continue;
        /* Match on source IP/port (SNAT/MASQUERADE) */
        if (nf_nat_rules[i].orig_ip == *ip &&
            (nf_nat_rules[i].orig_port == 0 || nf_nat_rules[i].orig_port == *port)) {
            *ip   = nf_nat_rules[i].new_ip;
            *port = nf_nat_rules[i].new_port;
            return 1;
        }
    }
    return 0;
}

/* ── Init ────────────────────────────────────────────────────────── */

void nf_init(void) {
    memset(nf_hooks, 0, sizeof(nf_hooks));
    memset(nf_rules, 0, sizeof(nf_rules));
    memset(nf_nat_rules, 0, sizeof(nf_nat_rules));
    nf_conntrack_init();
    nf_helper_init();
    kprintf("[OK] Netfilter initialized\n");
}
#include "module.h"
module_init(nf_init);

/* ── Implement: netfilter_register ────────────────────── */
int netfilter_register(void *hook)
{
    if (!hook) return -EINVAL;
    return nf_register_hook((struct nf_hook_ops *)hook);
}
/* ── Implement: netfilter_unregister ──────────────────── */
int netfilter_unregister(void *hook)
{
    if (!hook) return -EINVAL;
    nf_unregister_hook((struct nf_hook_ops *)hook);
    return 0;
}
/* ── Implement: netfilter_hook ────────────────────────── */
int netfilter_hook(void *skb, void *dev, int dir)
{
    if (!skb) return -EINVAL;
    return nf_hook_slow(NFPROTO_IPV4, dir, (struct sk_buff *)skb,
                        (struct net_device *)dev, NULL);
}
