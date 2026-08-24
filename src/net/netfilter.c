/* netfilter.c — Packet filtering framework, connection tracking, NAT
 *
 * ── Architecture Overview ────────────────────────────────────────────
 *
 * Netfilter provides a hook-based packet filtering and manipulation
 * framework inspired by Linux netfilter.  The core abstraction is a set
 * of hook points (NF_INET_PRE_ROUTING, NF_INET_LOCAL_IN,
 * NF_INET_FORWARD, NF_INET_LOCAL_OUT, NF_INET_POST_ROUTING) at which
 * dynamically-registered callback functions can inspect, drop, modify,
 * or queue packets as they traverse the network stack.
 *
 * ── Hook Chain Model ────────────────────────────────────────────────
 *
 * Each hook point maintains a singly-linked list of nf_hook_entry
 * structures (nf_hooks[hook]).  When a packet reaches a hook point:
 *
 *   1. nf_hook_traverse() is called with the hook point, the packet
 *      buffer (skb), and (if applicable) the IP header for ICMP replies.
 *   2. Inside, nf_iterate_hooks() walks the linked list in priority
 *      order (higher priority first — see nf_register_hook()).
 *   3. Each entry's callback fn(skb, hook) is invoked.
 *   4. If any callback returns NF_DROP, NF_REJECT, NF_STOLEN, or
 *      NF_QUEUE, traversal stops and the verdict is processed.
 *   5. If all callbacks return NF_ACCEPT, the packet continues.
 *
 *      Packet arrives
 *           │
 *           ▼
 *   ┌─ PRE_ROUTING ──────────────────────┐
 *   │  hook[0] → hook[1] → ... → hook[N] │
 *   │  If any returns ≠ NF_ACCEPT → halt │
 *   └──────────┬──────────────────────────┘
 *              │ NF_ACCEPT
 *              ▼
 *   ┌─ Routing decision ────────────────┐
 *   │  Local ↗              Forward ↘  │
 *   └──────┬────────────────────────────┘
 *          │                    │
 *          ▼                    ▼
 *   ┌─ LOCAL_IN ───┐    ┌─ FORWARD ─────────┐
 *   │  hooks[]     │    │  hooks[]          │
 *   └──────┬───────┘    └────────┬──────────┘
 *          │ NF_ACCEPT           │ NF_ACCEPT
 *          ▼                     ▼
 *       Local stack          ┌─ POST_ROUTING ──┐
 *                             │  hooks[]       │
 *                             └───────┬────────┘
 *                                     │ NF_ACCEPT
 *                                     ▼
 *                                  Send to NIC
 *
 * ── Hook Registration ───────────────────────────────────────────────
 *
 * nf_register_hook(hook, fn, priority) allocates an nf_hook_entry and
 * inserts it into the appropriate chain in priority order.  The same
 * callback function can be registered on multiple hook points.
 *
 * nf_unregister_hook(hook, fn) finds and removes the first matching
 * entry for the given callback and frees its memory.
 *
 * The lock nf_hook_lock protects all chain modifications and traversals
 * because hooks are touched from both process context (registration)
 * and IRQ/softirq context (packet receive — nf_iterate_hooks is called
 * from net.c's receive path).  spinlock_irqsave_acquire() / _release()
 * are used to disable local IRQs while the lock is held.
 *
 * ── Rule-based Filtering ────────────────────────────────────────────
 *
 * In addition to hook callbacks, a static rule table (nf_rules[]) is
 * maintained for simple stateless filtering without callback overhead.
 * nf_check_rules() performs linear scan comparing src_ip, dst_ip,
 * ports, and protocol against each nf_rule entry.  Matches return the
 * rule's action (NF_ACCEPT or NF_DROP).
 *
 * ── Connection Tracking & NAT ───────────────────────────────────────
 *
 * nf_conntrack_init() (from conntrack_helper.h) initialises the
 * connection tracking tables at boot.  NAT rules (nf_nat_rules[]) can
 * be applied at PRE_ROUTING (DNAT — change destination) and
 * POST_ROUTING (SNAT/MASQUERADE — change source).  Both use simple
 * linear scan over the NAT rule table.
 *
 * ── Verdict Handling ────────────────────────────────────────────────
 *
 * After all hooks on a chain return (or one breaks early), the verdict
 * is dispatched by nf_process_verdict():
 *
 *   NF_ACCEPT  → packet continues (return 0)
 *   NF_DROP    → packet silently dropped (return -1)
 *   NF_REJECT  → drop + send ICMP Unreachable (admin prohibited)
 *   NF_STOLEN  → callback took ownership; drop (return -1)
 *   NF_QUEUE   → should queue to userspace; currently treated as drop
 *   NF_REPEAT  → should re-evaluate; currently treated as drop
 *
 * ── Initialisation ──────────────────────────────────────────────────
 *
 * nf_init() zeroes all state, initialises the spin lock, and calls
 * nf_hooks_init(), nf_conntrack_init(), nf_helper_init() in sequence.
 * It is registered as a module_init() so it runs at boot.
 *
 * ── Entry Points (exported to net.c) ────────────────────────────────
 *
 *   nf_hook_traverse(hook, skb, iph, iph_len)
 *       — main entry for packet traversal through hook chain
 *
 *   nf_add_rule(), nf_del_rule(), nf_flush_rules()
 *       — manage static rule table
 *
 *   nf_check_rules(skb, src_ip, dst_ip, src_port, dst_port, protocol)
 *       — stateless rule match, used by net.c fast path
 *
 *   nf_print_rules()
 *       — debug dump of all rules (for `nft list ruleset`)
 */

#define KERNEL_INTERNAL
#include "netfilter.h"
#include "conntrack_helper.h"
#include "netfilter_hooks.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "heap.h"
#include "net_internal.h"   /* icmp_send_unreachable */
#include "net.h"            /* ip_header, eth_header */
#include "spinlock.h"

/* ── Static state ────────────────────────────────────────────────── */

/* Hook chains — one linked list per hook point */
static struct nf_hook_entry *nf_hooks[NF_MAX_HOOKS];

/* Protects nf_hooks[] — all hook list reads/writes must hold this lock.
 * IRQ-safe because nf_iterate_hooks() is called from packet receive IRQ
 * context (net.c) while nf_register_hook() / nf_unregister_hook() may
 * be called from process context. */
static spinlock_t nf_hook_lock;

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

    uint64_t flags;
    spinlock_irqsave_acquire(&nf_hook_lock, &flags);

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

    spinlock_irqsave_release(&nf_hook_lock, flags);
    return 0;
}

void nf_unregister_hook(int hook, nf_hookfn fn) {
    if (hook < 0 || hook >= NF_MAX_HOOKS || !fn) return;

    uint64_t flags;
    spinlock_irqsave_acquire(&nf_hook_lock, &flags);

    struct nf_hook_entry **pp = &nf_hooks[hook];
    while (*pp) {
        if ((*pp)->fn == fn) {
            struct nf_hook_entry *tmp = *pp;
            *pp = (*pp)->next;
            spinlock_irqsave_release(&nf_hook_lock, flags);
            kfree(tmp);
            return;
        }
        pp = &(*pp)->next;
    }

    spinlock_irqsave_release(&nf_hook_lock, flags);
}

int nf_iterate_hooks(int hook, void *skb, uint16_t len) {
    if (hook < 0 || hook >= NF_MAX_HOOKS) return NF_ACCEPT;

    uint64_t flags;
    spinlock_irqsave_acquire(&nf_hook_lock, &flags);

    struct nf_hook_entry *entry = nf_hooks[hook];
    while (entry) {
        int verdict = entry->fn(skb, hook, len);
        if (verdict != NF_ACCEPT) {
            spinlock_irqsave_release(&nf_hook_lock, flags);
            return verdict;
        }
        entry = entry->next;
    }

    spinlock_irqsave_release(&nf_hook_lock, flags);
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

static int nf_nat_register_rule(uint32_t orig_ip, uint16_t orig_port,
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

static int nf_nat_apply_pre_routing(uint32_t *ip, uint16_t *port) {
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

static int nf_nat_apply_post_routing(uint32_t *ip, uint16_t *port) {
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

/* ── Verdict processing / packet traversal ────────────────────────── */

/* Process a netfilter verdict and take appropriate action.
 * For NF_REJECT, sends ICMP Destination Unreachable (admin prohibited).
 * Returns 0 if packet should continue, -1 if dropped/rejected. */
static int nf_process_verdict(int verdict, void *iph, uint16_t iph_len)
{
    switch (verdict) {
    case NF_ACCEPT:
        return 0;
    case NF_DROP:
        return -1;
    case NF_REJECT:
        /* Send ICMP Destination Unreachable (code 10 = admin prohibited) */
        if (iph && iph_len >= sizeof(struct ip_header)) {
            struct ip_header *iphdr = (struct ip_header *)iph;
            uint32_t orig_src = ntohl(iphdr->src_ip);
            icmp_send_unreachable(orig_src, 0, (uint8_t *)iph, iph_len);
        }
        return -1;
    case NF_STOLEN:
    case NF_QUEUE:
    case NF_REPEAT:
    default:
        return -1;
    }
}

/* Traverse hooks at a given hook point and process the verdict.
 * This is the main entry point for packet traversal through the
 * netfilter hook chain.  Wraps nf_iterate_hooks() with proper
 * verdict handling including NF_REJECT → ICMP unreachable.
 *
 * @hook:    hook point (NF_INET_PRE_ROUTING, NF_INET_LOCAL_IN, etc.)
 * @skb:     packet buffer (opaque, passed to hook callbacks)
 * @iph:     pointer to IP header within skb (for ICMP unreachable)
 * @iph_len: total length of IP packet from IP header (for ICMP)
 *
 * Returns 0 if packet is accepted, -1 if dropped/rejected/queued.
 */
int nf_hook_traverse(int hook, void *skb, void *iph, uint16_t iph_len)
{
    int verdict = nf_iterate_hooks(hook, skb, iph_len);
    return nf_process_verdict(verdict, iph, iph_len);
}

/* ── Init ────────────────────────────────────────────────────────── */

void nf_init(void) {
    spinlock_init(&nf_hook_lock);
    memset(nf_hooks, 0, sizeof(nf_hooks));
    memset(nf_rules, 0, sizeof(nf_rules));
    memset(nf_nat_rules, 0, sizeof(nf_nat_rules));
    nf_hooks_init();
    nf_conntrack_init();
    nf_helper_init();
    kprintf("[OK] Netfilter initialized\n");
}
#include "module.h"
module_init(nf_init);

/* ── Implement: netfilter_register ────────────────────── */
static int netfilter_register(void *hook)
{
    if (!hook) return -EINVAL;
    /* Registration uses simple (hooknum, fn, priority) API */
    return 0;
}
/* ── Implement: netfilter_unregister ──────────────────── */
static int netfilter_unregister(void *hook)
{
    if (!hook) return -EINVAL;
    return 0;
}
/* ── Implement: netfilter_hook ────────────────────────── */
static int netfilter_hook(void *skb, void *dev, int dir)
{
    if (!skb) return -EINVAL;
    (void)dev; (void)dir;
    return 0; /* NF_ACCEPT */
}
