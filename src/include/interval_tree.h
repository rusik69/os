#ifndef INTERVAL_TREE_H
#define INTERVAL_TREE_H

#include "types.h"

/* Interval tree node – embedded red-black tree with augmented max_last. */
struct interval_tree_node {
    uint64_t start;
    uint64_t last;       /* inclusive endpoint */
    uint64_t max_last;   /* max(last) in subtree (augmented) */

    /* RB-tree links */
    struct interval_tree_node *rb_left;
    struct interval_tree_node *rb_right;
    struct interval_tree_node *rb_parent;
    int                        rb_color; /* 0 = red, 1 = black */
};

struct interval_tree {
    struct interval_tree_node *root;
    int                        count;
};

/* Initialise an interval tree (flat). */
void interval_tree_init(struct interval_tree *tree);

/* Insert a node. The node must not already be present. */
void interval_tree_insert(struct interval_tree *tree,
                          struct interval_tree_node *node);

/* Remove a node. */
void interval_tree_remove(struct interval_tree *tree,
                          struct interval_tree_node *node);

/* Find any node whose interval overlaps [start, last].
 * Returns NULL if none found. */
struct interval_tree_node *interval_tree_search(
    struct interval_tree *tree, uint64_t start, uint64_t last);

/* Find all nodes overlapping [start, last] via callback. */
void interval_tree_search_all(struct interval_tree *tree,
                              uint64_t start, uint64_t last,
                              void (*fn)(struct interval_tree_node *node, void *data),
                              void *ctx);

static inline int interval_tree_count(struct interval_tree *tree) {
    return tree->count;
}

void interval_tree_init_global(void);

#endif /* INTERVAL_TREE_H */
