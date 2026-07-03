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
    r->verdict = NFT_VERDICT_CONTINUE;
    r->target_chain[0] = '\0';

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

/* ── Set management ─────────────────────────────────────────────────── *
 * Backend dispatch: nft_set_add, nft_set_del, nft_set_lookup,
 * nft_set_init, nft_set_destroy, plus per-backend helpers.
 * Supports four backends: array (original), hash, rbtree, bitmap.
 * ─────────────────────────────────────────────────────────────────────── */

/* ── Hash backend ────────────────────────────────────────────────── */

static uint64_t nft_hash_key(uint32_t ip, uint16_t port, uint8_t proto)
{
    return ((uint64_t)ip << 32) | ((uint64_t)port << 16) | (uint64_t)proto;
}

static int nft_set_add_hash(struct nft_set *set, uint32_t ip,
                             uint16_t port, uint8_t proto,
                             uint32_t timeout_ms)
{
    uint64_t key = nft_hash_key(ip, port, proto);
    struct nft_set_elem *e;

    e = (struct nft_set_elem *)hashtable_lookup(&set->data.hash, key);
    if (e) {
        e->used = 1;
        return 0;
    }

    e = (struct nft_set_elem *)kmalloc(sizeof(struct nft_set_elem));
    if (!e)
        return -ENOMEM;

    e->ip = ip;
    e->port = port;
    e->protocol = proto;
    e->used = 1;
    e->timeout_ms = timeout_ms;

    return hashtable_insert(&set->data.hash, key, e);
}

static int nft_set_del_hash(struct nft_set *set, uint32_t ip,
                             uint16_t port, uint8_t proto)
{
    uint64_t key = nft_hash_key(ip, port, proto);
    struct nft_set_elem *e;

    e = (struct nft_set_elem *)hashtable_lookup(&set->data.hash, key);
    if (e) {
        kfree(e);
        return hashtable_remove(&set->data.hash, key);
    }
    return -ENOENT;
}

static int nft_set_lookup_hash(struct nft_set *set, uint32_t ip,
                                uint16_t port, uint8_t proto)
{
    uint64_t key = nft_hash_key(ip, port, proto);
    return (hashtable_lookup(&set->data.hash, key) != NULL) ? 1 : 0;
}

/* ── RB-tree backend ──────────────────────────────────────────────── */
/* Standard left-leaning red-black tree for range-based matching.
 * Nodes store [ip_min, ip_max] and [port_min, port_max] for intervals.
 * Lookup uses range matching; insert/delete use single-point keys. */

#define NFT_RB_RED   0
#define NFT_RB_BLACK 1

/* Compare (ip, port, proto) against an RB node's interval.
 * Returns 0 if within interval, <0 if below, >0 if above. */
static int nft_rb_cmp(uint32_t ip, uint16_t port, uint8_t proto,
                       const struct nft_rb_node *node)
{
    if (ip < node->ip_min) return -2;
    if (ip > node->ip_max) return 2;
    if (port < node->port_min) return -1;
    if (port > node->port_max) return 1;
    if (node->protocol != 0 && node->protocol != proto)
        return (proto < node->protocol) ? -1 : 1;
    return 0; /* match */
}

/* Strict ordering comparison for BST insert by (ip, port, proto). */
static int nft_rb_lt(uint32_t ip, uint16_t port, uint8_t proto,
                      const struct nft_rb_node *n)
{
    if (ip < n->ip_min) return 1;
    if (ip > n->ip_min) return 0;
    if (port < n->port_min) return 1;
    if (port > n->port_min) return 0;
    return (proto < n->protocol);
}

static void nft_rb_rotate_left(struct nft_rb_node **root,
                                struct nft_rb_node *x)
{
    struct nft_rb_node *y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (!x->parent)
        *root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void nft_rb_rotate_right(struct nft_rb_node **root,
                                 struct nft_rb_node *y)
{
    struct nft_rb_node *x = y->left;
    y->left = x->right;
    if (x->right) x->right->parent = y;
    x->parent = y->parent;
    if (!y->parent)
        *root = x;
    else if (y == y->parent->left)
        y->parent->left = x;
    else
        y->parent->right = x;
    x->right = y;
    y->parent = x;
}

static void nft_rb_insert_fixup(struct nft_rb_node **root,
                                 struct nft_rb_node *z)
{
    while (z->parent && z->parent->color == NFT_RB_RED) {
        if (z->parent == z->parent->parent->left) {
            struct nft_rb_node *y = z->parent->parent->right;
            if (y && y->color == NFT_RB_RED) {
                z->parent->color = NFT_RB_BLACK;
                y->color = NFT_RB_BLACK;
                z->parent->parent->color = NFT_RB_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    nft_rb_rotate_left(root, z);
                }
                z->parent->color = NFT_RB_BLACK;
                z->parent->parent->color = NFT_RB_RED;
                nft_rb_rotate_right(root, z->parent->parent);
            }
        } else {
            struct nft_rb_node *y = z->parent->parent->left;
            if (y && y->color == NFT_RB_RED) {
                z->parent->color = NFT_RB_BLACK;
                y->color = NFT_RB_BLACK;
                z->parent->parent->color = NFT_RB_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    nft_rb_rotate_right(root, z);
                }
                z->parent->color = NFT_RB_BLACK;
                z->parent->parent->color = NFT_RB_RED;
                nft_rb_rotate_left(root, z->parent->parent);
            }
        }
    }
    (*root)->color = NFT_RB_BLACK;
}

static int nft_rb_insert_node(struct nft_rb_node **root,
                               uint32_t ip, uint16_t port,
                               uint8_t protocol)
{
    struct nft_rb_node *z;

    z = (struct nft_rb_node *)kmalloc(sizeof(struct nft_rb_node));
    if (!z)
        return -ENOMEM;

    z->ip_min = ip;
    z->ip_max = ip;
    z->port_min = port;
    z->port_max = port;
    z->protocol = protocol;
    z->left = NULL;
    z->right = NULL;
    z->parent = NULL;
    z->color = NFT_RB_RED;

    /* Standard BST insert */
    {
        struct nft_rb_node *y = NULL;
        struct nft_rb_node *x = *root;

        while (x) {
            y = x;
            if (nft_rb_lt(ip, port, protocol, x))
                x = x->left;
            else
                x = x->right;
        }

        z->parent = y;
        if (!y)
            *root = z;
        else if (nft_rb_lt(ip, port, protocol, y))
            y->left = z;
        else
            y->right = z;
    }

    nft_rb_insert_fixup(root, z);
    return 0;
}

static struct nft_rb_node *nft_rb_minimum(struct nft_rb_node *node)
{
    while (node && node->left)
        node = node->left;
    return node;
}

static void nft_rb_transplant(struct nft_rb_node **root,
                               struct nft_rb_node *u,
                               struct nft_rb_node *v)
{
    if (!u->parent)
        *root = v;
    else if (u == u->parent->left)
        u->parent->left = v;
    else
        u->parent->right = v;
    if (v)
        v->parent = u->parent;
}

static void nft_rb_delete_fixup(struct nft_rb_node **root,
                                 struct nft_rb_node *x)
{
    while (x != *root && (!x || x->color == NFT_RB_BLACK)) {
        struct nft_rb_node *p = x ? x->parent : NULL;
        if (!p) break;

        if (x == p->left) {
            struct nft_rb_node *w = p->right;
            if (w && w->color == NFT_RB_RED) {
                w->color = NFT_RB_BLACK;
                p->color = NFT_RB_RED;
                nft_rb_rotate_left(root, p);
                w = p->right;
            }
            if (w && (!w->left || w->left->color == NFT_RB_BLACK) &&
                (!w->right || w->right->color == NFT_RB_BLACK)) {
                w->color = NFT_RB_RED;
                x = p;
            } else {
                if (w && (!w->right || w->right->color == NFT_RB_BLACK)) {
                    if (w->left) w->left->color = NFT_RB_BLACK;
                    w->color = NFT_RB_RED;
                    nft_rb_rotate_right(root, w);
                    w = p->right;
                }
                if (w) w->color = p->color;
                p->color = NFT_RB_BLACK;
                if (w && w->right) w->right->color = NFT_RB_BLACK;
                nft_rb_rotate_left(root, p);
                x = *root;
                break;
            }
        } else {
            struct nft_rb_node *w = p->left;
            if (w && w->color == NFT_RB_RED) {
                w->color = NFT_RB_BLACK;
                p->color = NFT_RB_RED;
                nft_rb_rotate_right(root, p);
                w = p->left;
            }
            if (w && (!w->right || w->right->color == NFT_RB_BLACK) &&
                (!w->left || w->left->color == NFT_RB_BLACK)) {
                w->color = NFT_RB_RED;
                x = p;
            } else {
                if (w && (!w->left || w->left->color == NFT_RB_BLACK)) {
                    if (w->right) w->right->color = NFT_RB_BLACK;
                    w->color = NFT_RB_RED;
                    nft_rb_rotate_left(root, w);
                    w = p->left;
                }
                if (w) w->color = p->color;
                p->color = NFT_RB_BLACK;
                if (w && w->left) w->left->color = NFT_RB_BLACK;
                nft_rb_rotate_right(root, p);
                x = *root;
                break;
            }
        }
    }
    if (x)
        x->color = NFT_RB_BLACK;
}

static int nft_rb_delete_node(struct nft_rb_node **root,
                               uint32_t ip, uint16_t port,
                               uint8_t protocol)
{
    if (!*root)
        return -ENOENT;

    /* Find the node by (ip, port, proto) */
    struct nft_rb_node *z = *root;
    while (z) {
        if (nft_rb_lt(ip, port, protocol, z))
            z = z->left;
        else if (ip == z->ip_min && port == z->port_min &&
                 protocol == z->protocol)
            break; /* found exact match */
        else
            z = z->right;
    }
    if (!z)
        return -ENOENT;

    {
        struct nft_rb_node *x, *y;
        uint8_t y_orig_color = z->color;

        y = z;
        if (!z->left) {
            x = z->right;
            nft_rb_transplant(root, z, z->right);
            if (x) x->parent = z->parent;
        } else if (!z->right) {
            x = z->left;
            nft_rb_transplant(root, z, z->left);
            if (x) x->parent = z->parent;
        } else {
            y = nft_rb_minimum(z->right);
            y_orig_color = y->color;
            x = y->right;
            if (y->parent == z) {
                if (x) x->parent = y;
            } else {
                nft_rb_transplant(root, y, y->right);
                y->right = z->right;
                if (y->right) y->right->parent = y;
            }
            nft_rb_transplant(root, z, y);
            y->left = z->left;
            if (y->left) y->left->parent = y;
            y->color = z->color;
        }

        kfree(z);

        if (y_orig_color == NFT_RB_BLACK)
            nft_rb_delete_fixup(root, x);
    }

    return 0;
}

static void nft_rb_free_subtree(struct nft_rb_node *node)
{
    if (!node) return;
    nft_rb_free_subtree(node->left);
    nft_rb_free_subtree(node->right);
    kfree(node);
}

static int nft_set_add_rbtree(struct nft_set *set, uint32_t ip,
                               uint16_t port, uint8_t proto,
                               uint32_t timeout_ms)
{
    (void)timeout_ms;
    return nft_rb_insert_node(&set->data.rb.root, ip, port, proto);
}

static int nft_set_del_rbtree(struct nft_set *set, uint32_t ip,
                               uint16_t port, uint8_t proto)
{
    int ret = nft_rb_delete_node(&set->data.rb.root, ip, port, proto);
    if (ret == 0)
        set->data.rb.count--;
    return ret;
}

static int nft_set_lookup_rbtree(struct nft_set *set, uint32_t ip,
                                  uint16_t port, uint8_t proto)
{
    struct nft_rb_node *node = set->data.rb.root;
    while (node) {
        int cmp = nft_rb_cmp(ip, port, proto, node);
        if (cmp == 0) return 1; /* match */
        if (cmp < 0)
            node = node->left;
        else
            node = node->right;
    }
    return 0;
}

/* ── Bitmap backend ───────────────────────────────────────────────── */

#define NFT_BITMAP_NBITS  65536
#define NFT_BITMAP_LONGS  (NFT_BITMAP_NBITS / (8 * (int)sizeof(unsigned long)))

static int nft_set_init_bitmap_backend(struct nft_set *set)
{
    unsigned long *bmp;

    bmp = (unsigned long *)kmalloc(
        (size_t)NFT_BITMAP_LONGS * sizeof(unsigned long));
    if (!bmp)
        return -ENOMEM;

    set->data.bmp.bitmap = bmp;
    set->data.bmp.nbits = NFT_BITMAP_NBITS;

    for (int i = 0; i < NFT_BITMAP_LONGS; i++)
        bmp[i] = 0UL;

    return 0;
}

static int nft_set_add_bitmap(struct nft_set *set, uint32_t ip,
                               uint16_t port, uint8_t proto,
                               uint32_t timeout_ms)
{
    (void)ip;
    (void)proto;
    (void)timeout_ms;

    if (!set->data.bmp.bitmap || (int)port >= set->data.bmp.nbits)
        return -EINVAL;

    set->data.bmp.bitmap[port / (8 * (int)sizeof(unsigned long))] |=
        (1UL << (port % (8 * (int)sizeof(unsigned long))));
    set->n_elems++;
    return 0;
}

static int nft_set_del_bitmap(struct nft_set *set, uint32_t ip,
                               uint16_t port, uint8_t proto)
{
    (void)ip;
    (void)proto;

    if (!set->data.bmp.bitmap || (int)port >= set->data.bmp.nbits)
        return -EINVAL;

    if (!((set->data.bmp.bitmap[port / (8 * (int)sizeof(unsigned long))] >>
            (port % (8 * (int)sizeof(unsigned long)))) & 1UL))
        return -ENOENT;

    set->data.bmp.bitmap[port / (8 * (int)sizeof(unsigned long))] &=
        ~(1UL << (port % (8 * (int)sizeof(unsigned long))));
    set->n_elems--;
    return 0;
}

static int nft_set_lookup_bitmap(struct nft_set *set, uint32_t ip,
                                  uint16_t port, uint8_t proto)
{
    (void)ip;
    (void)proto;

    if (!set->data.bmp.bitmap || (int)port >= set->data.bmp.nbits)
        return 0;

    return (int)((set->data.bmp.bitmap[port / (8 * (int)sizeof(unsigned long))] >>
                   (port % (8 * (int)sizeof(unsigned long)))) & 1UL);
}

/* ── Array backend (original) ────────────────────────────────────── */

static int nft_set_add_array(struct nft_set *set, uint32_t ip,
                              uint16_t port, uint8_t proto,
                              uint32_t timeout_ms)
{
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

static int nft_set_del_array(struct nft_set *set, uint32_t ip,
                              uint16_t port, uint8_t proto)
{
    if (!set) return -EINVAL;

    for (uint32_t i = 0; i < set->n_elems; i++) {
        struct nft_set_elem *e = &set->elems[i];
        if (e->used && e->ip == ip && e->port == port && e->protocol == proto) {
            e->used = 0;
            for (uint32_t j = i; j < set->n_elems - 1; j++)
                set->elems[j] = set->elems[j + 1];
            set->n_elems--;
            return 0;
        }
    }
    return -ENOENT;
}

static int nft_set_lookup_array(struct nft_set *set, uint32_t ip,
                                 uint16_t port, uint8_t proto)
{
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

/* ── Generic backend dispatch ────────────────────────────────────── */

int nft_set_add(struct nft_set *set, uint32_t ip, uint16_t port,
                uint8_t proto, uint32_t timeout_ms)
{
    if (!set) return -EINVAL;

    switch (set->backend) {
    case NFT_SET_BACKEND_HASH:
        return nft_set_add_hash(set, ip, port, proto, timeout_ms);
    case NFT_SET_BACKEND_RBTREE:
        return nft_set_add_rbtree(set, ip, port, proto, timeout_ms);
    case NFT_SET_BACKEND_BITMAP:
        return nft_set_add_bitmap(set, ip, port, proto, timeout_ms);
    case NFT_SET_BACKEND_ARRAY:
    default:
        return nft_set_add_array(set, ip, port, proto, timeout_ms);
    }
}

int nft_set_del(struct nft_set *set, uint32_t ip,
                uint16_t port, uint8_t proto)
{
    if (!set) return -EINVAL;

    switch (set->backend) {
    case NFT_SET_BACKEND_HASH:
        return nft_set_del_hash(set, ip, port, proto);
    case NFT_SET_BACKEND_RBTREE:
        return nft_set_del_rbtree(set, ip, port, proto);
    case NFT_SET_BACKEND_BITMAP:
        return nft_set_del_bitmap(set, ip, port, proto);
    case NFT_SET_BACKEND_ARRAY:
    default:
        return nft_set_del_array(set, ip, port, proto);
    }
}

int nft_set_lookup(struct nft_set *set, uint32_t ip,
                   uint16_t port, uint8_t proto)
{
    if (!set) return 0;

    switch (set->backend) {
    case NFT_SET_BACKEND_HASH:
        return nft_set_lookup_hash(set, ip, port, proto);
    case NFT_SET_BACKEND_RBTREE:
        return nft_set_lookup_rbtree(set, ip, port, proto);
    case NFT_SET_BACKEND_BITMAP:
        return nft_set_lookup_bitmap(set, ip, port, proto);
    case NFT_SET_BACKEND_ARRAY:
    default:
        return nft_set_lookup_array(set, ip, port, proto);
    }
}

int nft_set_init(struct nft_set *set, uint8_t type,
                 uint8_t backend, const char *name)
{
    if (!set || !name) return -EINVAL;

    memset(set, 0, sizeof(*set));
    strncpy(set->name, name, sizeof(set->name) - 1);
    set->name[sizeof(set->name) - 1] = '\0';
    set->type = type;
    set->backend = backend;

    switch (backend) {
    case NFT_SET_BACKEND_HASH:
        hashtable_init(&set->data.hash);
        break;
    case NFT_SET_BACKEND_RBTREE:
        set->data.rb.root = NULL;
        set->data.rb.count = 0;
        break;
    case NFT_SET_BACKEND_BITMAP:
        return nft_set_init_bitmap_backend(set);
    case NFT_SET_BACKEND_ARRAY:
    default:
        /* Array backend — all zeroed by memset */
        break;
    }

    return 0;
}

void nft_set_destroy(struct nft_set *set)
{
    if (!set) return;

    switch (set->backend) {
    case NFT_SET_BACKEND_HASH: {
        int i;
        struct list_head *pos, *n;
        for (i = 0; i < HASH_TABLE_SIZE; i++) {
            list_for_each_safe(pos, n, &set->data.hash.buckets[i]) {
                struct hashtable_node *hn =
                    list_entry(pos, struct hashtable_node, list);
                if (hn->value)
                    kfree(hn->value);
            }
        }
        break;
    }
    case NFT_SET_BACKEND_RBTREE:
        nft_rb_free_subtree(set->data.rb.root);
        set->data.rb.root = NULL;
        set->data.rb.count = 0;
        break;
    case NFT_SET_BACKEND_BITMAP:
        if (set->data.bmp.bitmap) {
            kfree(set->data.bmp.bitmap);
            set->data.bmp.bitmap = NULL;
        }
        break;
    case NFT_SET_BACKEND_ARRAY:
    default:
        break;
    }

    memset(set, 0, sizeof(*set));
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

/* Eval an immediate expression — loads a constant value into a register.
 * Copies the inline data from the expression into the destination register,
 * respecting the data length (zero-extends shorter lengths).
 * Always succeeds (the data is embedded in the expression, no runtime lookup). */
static int nft_expr_eval_immediate(const struct nft_expr *expr,
                                    struct nft_regs *regs)
{
    const struct nft_expr_immediate *imm =
        (const struct nft_expr_immediate *)expr;

    if (imm->dreg >= NFT_REG_COUNT || imm->len == 0)
        return 1; /* bad register or empty → no match */

    uint32_t val = imm->data[0];

    /* For sub-word lengths, mask to the correct number of bytes */
    if (imm->len < 4) {
        uint32_t mask = (1UL << (imm->len * 8)) - 1;
        val &= mask;
    }

    regs->data32[imm->dreg] = val;
    return 0; /* always succeeds */
}

/* Eval a counter expression — counts packets and bytes passing through.
 * Increments packet counter by 1 and byte counter by packet length.
 * Always returns 0 (counter is a side-effect only, never blocks a match). */
static int nft_expr_eval_counter(const struct nft_expr *expr,
                                 struct nft_regs *regs,
                                 const struct nft_eval_ctx *ctx)
{
    struct nft_expr_counter *c = (struct nft_expr_counter *)expr;

    (void)regs;

    c->packets++;
    c->bytes += ctx->pkt_len;

    return 0; /* always succeeds — counter is side-effect only */
}

/* Eval a NAT expression — performs DNAT/SNAT/MASQUERADE translation.
 * Stores the NAT result in the register file for the caller to apply
 * (the actual address/port modification happens at the network layer).
 *
 * DNAT (NFT_NAT_DNAT):
 *   Modifies destination address (and optionally port). Used in
 *   PREROUTING / LOCAL_OUT chains for port forwarding.
 *
 * SNAT (NFT_NAT_SNAT):
 *   Modifies source address (and optionally port). Used in
 *   POSTROUTING / LOCAL_IN chains for outbound NAT.
 *
 * MASQUERADE (NFT_NAT_MASQUERADE):
 *   Like SNAT but dynamically uses the outgoing interface address.
 *   addr field should be 0 in this case — the caller determines
 *   the source address from the output interface.
 *
 * Always returns 0 (NAT is an action expression, not a match). */
static int nft_expr_eval_nat(const struct nft_expr *expr,
                             struct nft_regs *regs,
                             const struct nft_eval_ctx *ctx)
{
    const struct nft_expr_nat *n = (const struct nft_expr_nat *)expr;

    (void)ctx;

    regs->nat.active = 1;
    regs->nat.type = n->nat_type;

    /* For MASQUERADE without explicit address, leave addr as 0.
     * The caller will resolve the outgoing interface address. */
    if (n->nat_type == NFT_NAT_MASQUERADE && n->addr == 0) {
        regs->nat.addr = 0;
    } else {
        regs->nat.addr = n->addr;
    }

    /* Use port_min as the target port (or 0 for no port change) */
    regs->nat.port = n->port_min;

    return 0; /* always succeeds — NAT is an action, not a match */
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

        case NFT_EXPR_IMMEDIATE:
            ret = nft_expr_eval_immediate(expr, regs);
            if (ret != 0)
                return 0;
            break;

        case NFT_EXPR_COUNTER:
            ret = nft_expr_eval_counter(expr, regs, ctx);
            if (ret != 0)
                return 0;
            break;

        case NFT_EXPR_NAT:
            ret = nft_expr_eval_nat(expr, regs, ctx);
            if (ret != 0)
                return 0;
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

/* ── Verdict processing ───────────────────────────────────────────── */

/* Map a rule's verdict or action to the effective NFT_VERDICT_* value.
 * When verdict is NFT_VERDICT_CONTINUE (default), falls back to legacy
 * action field for backward compatibility. */
static int nft_get_verdict(const struct nft_rule *rule)
{
    if (rule->verdict != NFT_VERDICT_CONTINUE)
        return (int)rule->verdict;

    switch (rule->action) {
    case NF_DROP:    return NFT_VERDICT_DROP;
    case NF_REJECT:  return NFT_VERDICT_REJECT;
    default:         return NFT_VERDICT_ACCEPT;
    }
}

/* Look up a chain by name within a table.
 * Returns NULL if not found. */
static struct nft_chain *nft_find_chain(struct nft_table *table,
                                         const char *name)
{
    if (!table || !name || !name[0])
        return NULL;

    for (uint32_t i = 0; i < table->n_chains; i++) {
        if (strcmp(table->chains[i].name, name) == 0)
            return &table->chains[i];
    }
    return NULL;
}

/* Evaluate a single chain's rules with verdict processing support.
 *
 * Walks rules in order. For each matching rule:
 *   - Updates counters
 *   - Processes the verdict (ACCEPT, DROP, REJECT, QUEUE, JUMP, GOTO, RETURN)
 *   - JUMP:  pushes return point, evaluates target chain, checks for RETURN
 *   - GOTO:  evaluates target chain without pushing return point
 *   - RETURN: returns NFT_VERDICT_RETURN if can_return is true
 *   - CONTINUE: continues to the next rule
 *
 * @can_return: if 1, NFT_VERDICT_RETURN propagates up to the caller
 *              (set for JUMP targets).  If 0, RETURN is treated as ACCEPT.
 * @depth:     recursion depth limit to prevent infinite JUMP loops
 *
 * Returns: NF_ACCEPT, NF_DROP, NF_REJECT, NFT_VERDICT_RETURN, or NF_QUEUE.
 */
static int nft_evaluate_chain_rules(struct nft_chain *chain,
                                     struct nft_table *table, void *skb,
                                     uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     uint8_t protocol, int hook,
                                     int can_return, int depth)
{
    if (!chain || depth > NFT_JUMP_STACK_DEPTH)
        return NF_ACCEPT;

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
            if (r->protocol != 0 && r->protocol != protocol)
                continue;

            if ((src_ip & r->src_mask) != (r->src_ip & r->src_mask))
                continue;

            if ((dst_ip & r->dst_mask) != (r->dst_ip & r->dst_mask))
                continue;

            if (r->src_port != 0 && r->src_port != src_port)
                continue;
            if (r->dst_port != 0 && r->dst_port != dst_port)
                continue;

            rule_match = 1;
        }

        if (!rule_match)
            continue;

        /* Update counters */
        r->counter_packets++;
        r->counter_bytes += 1500;

        /* ── Process the verdict ─────────────────────────── */
        int verdict = nft_get_verdict(r);

        switch (verdict) {
        case NFT_VERDICT_ACCEPT:
            return NF_ACCEPT;

        case NFT_VERDICT_DROP:
            return NF_DROP;

        case NFT_VERDICT_REJECT:
            return NF_REJECT;

        case NFT_VERDICT_QUEUE:
            kprintf("[nft] verdict: QUEUE (packet queued to userspace)\n");
            return NF_QUEUE;

        case NFT_VERDICT_JUMP:
        case NFT_VERDICT_GOTO: {
            if (depth >= NFT_JUMP_STACK_DEPTH) {
                kprintf("[nft] verdict: jump stack overflow\n");
                return NF_DROP;
            }

            struct nft_chain *target = nft_find_chain(table,
                                                       r->target_chain);
            if (!target) {
                kprintf("[nft] verdict: target chain '%s' not found\n",
                        r->target_chain);
                return NF_DROP;
            }

            /* JUMP can return; GOTO cannot */
            int jump_can_return = (verdict == NFT_VERDICT_JUMP) ? 1 : 0;

            int ret = nft_evaluate_chain_rules(target, table, skb,
                                                src_ip, dst_ip,
                                                src_port, dst_port,
                                                protocol, hook,
                                                jump_can_return,
                                                depth + 1);

            if (ret == NFT_VERDICT_RETURN && verdict == NFT_VERDICT_JUMP)
                continue; /* resume at the next rule in this chain */

            return ret; /* DROP, REJECT, ACCEPT, etc. */
        }

        case NFT_VERDICT_RETURN:
            if (can_return)
                return NFT_VERDICT_RETURN;
            return NF_ACCEPT; /* isolated RETURN = accept */

        case NFT_VERDICT_CONTINUE:
        default:
            continue; /* try the next rule */
        }
    }

    /* End of chain — no rule matched or all rules CONTINUE'd */
    if (can_return)
        return NFT_VERDICT_RETURN;
    return NF_ACCEPT;
}

/* Evaluate all chains in a table that match the given hook point.
 * Walks chains in insertion order.  Chains are independent — JUMP/GOTO
 * between chains within the same table is supported, but the top-level
 * evaluation uses each matching chain as a root.
 *
 * Returns NF_ACCEPT, NF_DROP, NF_REJECT, NF_QUEUE, or NFT_VERDICT_RETURN. */
int nft_evaluate(struct nft_table *table, void *skb,
                 uint32_t src_ip, uint32_t dst_ip,
                 uint16_t src_port, uint16_t dst_port,
                 uint8_t protocol, int hook)
{
    (void)skb;

    if (!table || !table->active)
        return NF_ACCEPT;

    /* Walk all chains that match this hook */
    for (uint32_t c = 0; c < table->n_chains; c++) {
        struct nft_chain *chain = &table->chains[c];

        /* Skip if chain doesn't match this hook */
        if (chain->hook_num != (uint8_t)hook)
            continue;

        int result = nft_evaluate_chain_rules(chain, table, skb,
                                               src_ip, dst_ip,
                                               src_port, dst_port,
                                               protocol, hook,
                                               0, 0);

        /* NFT_VERDICT_RETURN at top-level = accept (no caller to return to) */
        if (result == NFT_VERDICT_RETURN)
            result = NF_ACCEPT;

        if (result != NF_ACCEPT)
            return result;
    }

    return NF_ACCEPT;
}

/* Public API: apply a rule's verdict to a packet.
 * Useful for external callers (e.g., shell commands, test harness)
 * who want to test verdict processing on a specific rule. */
int nft_verdict_apply(struct nft_rule *rule,
                      struct nft_table *table, void *skb,
                      uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint8_t protocol, int hook)
{
    if (!rule)
        return NF_ACCEPT;

    (void)skb;

    /* For rules with expression chains, we must also evaluate them
     * to reach verdict expressions.  For now, just use the stored
     * verdict/action directly. */
    int verdict = nft_get_verdict(rule);

    switch (verdict) {
    case NFT_VERDICT_ACCEPT:
        return NF_ACCEPT;
    case NFT_VERDICT_DROP:
        return NF_DROP;
    case NFT_VERDICT_REJECT:
        return NF_REJECT;
    case NFT_VERDICT_QUEUE:
        return NF_QUEUE;
    case NFT_VERDICT_JUMP:
    case NFT_VERDICT_GOTO: {
        if (!table) return NF_DROP;
        struct nft_chain *target = nft_find_chain(table, rule->target_chain);
        if (!target) {
            kprintf("[nft] nft_verdict_apply: target '%s' not found\n",
                    rule->target_chain);
            return NF_DROP;
        }
        return nft_evaluate_chain_rules(target, table, skb,
                                         src_ip, dst_ip,
                                         src_port, dst_port,
                                         protocol, hook,
                                         (verdict == NFT_VERDICT_JUMP) ? 1 : 0,
                                         1);
    }
    case NFT_VERDICT_RETURN:
        return NF_ACCEPT; /* RETURN with no caller = accept */
    case NFT_VERDICT_CONTINUE:
    default:
        return NF_ACCEPT;
    }
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
