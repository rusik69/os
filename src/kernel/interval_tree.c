#include "interval_tree.h"
#include "printf.h"
#include "kernel.h"

/* Colour constants */
#define RB_RED   0
#define RB_BLACK 1

static inline int is_red(struct interval_tree_node *n)
{
    return n && n->rb_color == RB_RED;
}

static inline int is_black(struct interval_tree_node *n)
{
    return !n || n->rb_color == RB_BLACK;
}

static inline uint64_t max3(uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t m = a > b ? a : b;
    return m > c ? m : c;
}

/* Update the augmented max_last for a node from its children. */
static void update_max_last(struct interval_tree_node *node)
{
    node->max_last = node->last;
    if (node->rb_left && node->rb_left->max_last > node->max_last)
        node->max_last = node->rb_left->max_last;
    if (node->rb_right && node->rb_right->max_last > node->max_last)
        node->max_last = node->rb_right->max_last;
}

/* Rotate left */
static void rotate_left(struct interval_tree *tree,
                         struct interval_tree_node *x)
{
    struct interval_tree_node *y = x->rb_right;
    x->rb_right = y->rb_left;
    if (y->rb_left)
        y->rb_left->rb_parent = x;
    y->rb_parent = x->rb_parent;
    if (!x->rb_parent)
        tree->root = y;
    else if (x == x->rb_parent->rb_left)
        x->rb_parent->rb_left = y;
    else
        x->rb_parent->rb_right = y;
    y->rb_left = x;
    x->rb_parent = y;

    update_max_last(x);
    update_max_last(y);
}

/* Rotate right */
static void rotate_right(struct interval_tree *tree,
                          struct interval_tree_node *x)
{
    struct interval_tree_node *y = x->rb_left;
    x->rb_left = y->rb_right;
    if (y->rb_right)
        y->rb_right->rb_parent = x;
    y->rb_parent = x->rb_parent;
    if (!x->rb_parent)
        tree->root = y;
    else if (x == x->rb_parent->rb_right)
        x->rb_parent->rb_right = y;
    else
        x->rb_parent->rb_left = y;
    y->rb_right = x;
    x->rb_parent = y;

    update_max_last(x);
    update_max_last(y);
}

/* Insert a node into the interval tree. */
void interval_tree_insert(struct interval_tree *tree,
                           struct interval_tree_node *node)
{
    struct interval_tree_node *parent = NULL;
    struct interval_tree_node **link = &tree->root;

    /* Standard BST insert keyed by start, using > to break ties */
    while (*link) {
        parent = *link;
        if (node->start < parent->start)
            link = &parent->rb_left;
        else
            link = &parent->rb_right;
    }

    node->rb_parent = parent;
    node->rb_left = node->rb_right = NULL;
    node->rb_color = RB_RED;
    *link = node;

    /* Update augmented values on the path */
    {
        struct interval_tree_node *p = node;
        while (p) {
            update_max_last(p);
            p = p->rb_parent;
        }
    }

    /* Fix red-black violations */
    while (is_red(node->rb_parent)) {
        struct interval_tree_node *grandpa = node->rb_parent->rb_parent;
        if (node->rb_parent == grandpa->rb_left) {
            struct interval_tree_node *uncle = grandpa->rb_right;
            if (is_red(uncle)) {
                node->rb_parent->rb_color = RB_BLACK;
                uncle->rb_color = RB_BLACK;
                grandpa->rb_color = RB_RED;
                node = grandpa;
            } else {
                if (node == node->rb_parent->rb_right) {
                    node = node->rb_parent;
                    rotate_left(tree, node);
                }
                node->rb_parent->rb_color = RB_BLACK;
                grandpa->rb_color = RB_RED;
                rotate_right(tree, grandpa);
            }
        } else {
            struct interval_tree_node *uncle = grandpa->rb_left;
            if (is_red(uncle)) {
                node->rb_parent->rb_color = RB_BLACK;
                uncle->rb_color = RB_BLACK;
                grandpa->rb_color = RB_RED;
                node = grandpa;
            } else {
                if (node == node->rb_parent->rb_left) {
                    node = node->rb_parent;
                    rotate_right(tree, node);
                }
                node->rb_parent->rb_color = RB_BLACK;
                grandpa->rb_color = RB_RED;
                rotate_left(tree, grandpa);
            }
        }
    }
    tree->root->rb_color = RB_BLACK;
    tree->count++;
}

/* Remove a node from the interval tree. */
void interval_tree_remove(struct interval_tree *tree,
                           struct interval_tree_node *node)
{
    struct interval_tree_node *child, *parent;
    int color;
    int child_is_left = 0; /* track which side child is on relative to parent */

    if (!node->rb_left || !node->rb_right) {
        /* Node has at most one child */
        child = node->rb_left ? node->rb_left : node->rb_right;
        parent = node->rb_parent;
        color = node->rb_color;

        if (child)
            child->rb_parent = parent;
        if (!parent)
            tree->root = child;
        else if (node == parent->rb_left) {
            parent->rb_left = child;
            child_is_left = 1;
        } else {
            parent->rb_right = child;
            child_is_left = 0;
        }
    } else {
        /* Node has two children – find successor */
        struct interval_tree_node *succ = node->rb_right;
        while (succ->rb_left)
            succ = succ->rb_left;
        struct interval_tree_node *old_parent = node->rb_parent;
        struct interval_tree_node *old_left = node->rb_left;
        struct interval_tree_node *old_right = node->rb_right;
        int old_color = node->rb_color;

        /* Splice succ out */
        child = succ->rb_right;
        parent = succ->rb_parent;
        color = succ->rb_color;

        if (child)
            child->rb_parent = parent;
        if (!parent)
            tree->root = child;
        else if (succ == parent->rb_left) {
            parent->rb_left = child;
            child_is_left = 1;
        } else {
            parent->rb_right = child;
            child_is_left = 0;
        }

        /* Move succ to node's position */
        succ->rb_parent = old_parent;
        if (!old_parent)
            tree->root = succ;
        else if (node == old_parent->rb_left)
            old_parent->rb_left = succ;
        else
            old_parent->rb_right = succ;
        succ->rb_left = old_left;
        if (old_left)
            old_left->rb_parent = succ;
        /* Avoid self-loop when succ was node's immediate right child */
        if (old_right != succ) {
            succ->rb_right = old_right;
            if (old_right)
                old_right->rb_parent = succ;
        }
        /* If succ was node->rb_right, succ->rb_right stays unchanged */
        succ->rb_color = old_color;
    }

    /* Update max_last along the path */
    {
        struct interval_tree_node *p = parent;
        while (p) {
            update_max_last(p);
            p = p->rb_parent;
        }
    }

    /* Fix red-black if we removed a black node */
    if (color == RB_BLACK) {
        /* Simple case: child is red, colour it black */
        if (is_red(child)) {
            if (child)
                child->rb_color = RB_BLACK;
        } else {
            /* Full RB delete fixup – use explicit side tracking */
            struct interval_tree_node *x = child;
            struct interval_tree_node *xp = parent;
            int x_is_left = child_is_left;

            while (x != tree->root && is_black(x)) {
                struct interval_tree_node *w;

                if (x_is_left) {
                    w = xp->rb_right;
                    if (is_red(w)) {
                        w->rb_color = RB_BLACK;
                        xp->rb_color = RB_RED;
                        rotate_left(tree, xp);
                        w = xp->rb_right;
                    }
                    if (!w || (is_black(w->rb_left) && is_black(w->rb_right))) {
                        if (w)
                            w->rb_color = RB_RED;
                        x = xp;
                        xp = xp->rb_parent;
                        x_is_left = (xp && x == xp->rb_left);
                    } else {
                        if (is_black(w->rb_right)) {
                            if (w->rb_left)
                                w->rb_left->rb_color = RB_BLACK;
                            w->rb_color = RB_RED;
                            rotate_right(tree, w);
                            w = xp->rb_right;
                        }
                        w->rb_color = xp->rb_color;
                        xp->rb_color = RB_BLACK;
                        if (w->rb_right)
                            w->rb_right->rb_color = RB_BLACK;
                        rotate_left(tree, xp);
                        x = tree->root;
                        break;
                    }
                } else {
                    w = xp->rb_left;
                    if (is_red(w)) {
                        w->rb_color = RB_BLACK;
                        xp->rb_color = RB_RED;
                        rotate_right(tree, xp);
                        w = xp->rb_left;
                    }
                    if (!w || (is_black(w->rb_right) && is_black(w->rb_left))) {
                        if (w)
                            w->rb_color = RB_RED;
                        x = xp;
                        xp = xp->rb_parent;
                        x_is_left = (xp && x == xp->rb_left);
                    } else {
                        if (is_black(w->rb_left)) {
                            if (w->rb_right)
                                w->rb_right->rb_color = RB_BLACK;
                            w->rb_color = RB_RED;
                            rotate_left(tree, w);
                            w = xp->rb_left;
                        }
                        w->rb_color = xp->rb_color;
                        xp->rb_color = RB_BLACK;
                        if (w->rb_left)
                            w->rb_left->rb_color = RB_BLACK;
                        rotate_right(tree, xp);
                        x = tree->root;
                        break;
                    }
                }
            }
            if (x)
                x->rb_color = RB_BLACK;
        }
        if (tree->root)
            tree->root->rb_color = RB_BLACK;
    }

    tree->count--;
}

/* Search for any interval overlapping [start, last]. */
struct interval_tree_node *interval_tree_search(
    struct interval_tree *tree, uint64_t start, uint64_t last)
{
    struct interval_tree_node *node = tree->root;

    while (node) {
        if (node->max_last < start) {
            /* No overlapping interval in this subtree */
            return NULL;
        }

        /* Check left child (may have overlapping intervals) */
        if (node->rb_left && node->rb_left->max_last >= start) {
            node = node->rb_left;
            continue;
        }

        /* Check current node */
        if (node->start <= last && node->last >= start)
            return node;

        /* Try right child */
        node = node->rb_right;
    }
    return NULL;
}

/* Recursive helper for search_all */
static void search_all_rec(struct interval_tree_node *node,
                           uint64_t start, uint64_t last,
                           void (*fn)(struct interval_tree_node *, void *),
                           void *ctx)
{
    if (!node || node->max_last < start)
        return;

    /* Left subtree */
    search_all_rec(node->rb_left, start, last, fn, ctx);

    /* This node */
    if (node->start <= last && node->last >= start)
        fn(node, ctx);

    /* Right subtree */
    search_all_rec(node->rb_right, start, last, fn, ctx);
}

void interval_tree_search_all(struct interval_tree *tree,
                              uint64_t start, uint64_t last,
                              void (*fn)(struct interval_tree_node *, void *),
                              void *ctx)
{
    search_all_rec(tree->root, start, last, fn, ctx);
}

void interval_tree_init(struct interval_tree *tree)
{
    tree->root  = NULL;
    tree->count = 0;
}

void interval_tree_init_global(void)
{
    kprintf("[OK] interval_tree: Interval tree initialised\n");
}

/* ── Stub: interval_tree_iter_first ─────────────────────────────── */
void* interval_tree_iter_first(void *root, uint64_t start, uint64_t last)
{
    (void)root;
    (void)start;
    (void)last;
    kprintf("[itree] interval_tree_iter_first: not yet implemented\n");
    return 0;
}
