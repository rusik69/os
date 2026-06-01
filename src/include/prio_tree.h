#ifndef PRIO_TREE_H
#define PRIO_TREE_H

#include "types.h"

/*
 * Priority search tree – hybrid of radix tree and heap.
 * Each node has a (prio, index) pair.  The tree supports
 * insertion, removal, and searching for the maximum-priority
 * node with index in a given range.
 */

struct prio_tree_node {
    uint64_t prio;
    uint64_t index;

    struct prio_tree_node *left;
    struct prio_tree_node *right;
    struct prio_tree_node *parent;
};

struct prio_tree_root {
    struct prio_tree_node *prio_tree_node;
    int                    count;
};

/* Initialise an empty priority search tree. */
void prio_tree_init(struct prio_tree_root *root);

/* Insert a node. */
int prio_tree_insert(struct prio_tree_root *root,
                     struct prio_tree_node *node);

/* Remove a node. */
void prio_tree_remove(struct prio_tree_root *root,
                      struct prio_tree_node *node);

/* Find the node with the highest priority in [min_index, max_index].
 * Returns NULL if none found. */
struct prio_tree_node *prio_tree_search(struct prio_tree_root *root,
                                        uint64_t min_index,
                                        uint64_t max_index);

/* Iterate all nodes in priority order (descending). */
void prio_tree_iterate(struct prio_tree_root *root,
                       void (*fn)(struct prio_tree_node *, void *),
                       void *ctx);

static inline int prio_tree_count(struct prio_tree_root *root) {
    return root->count;
}

void prio_tree_init_global(void);

#endif /* PRIO_TREE_H */
