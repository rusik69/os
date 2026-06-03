#include "prio_tree.h"
#include "printf.h"
#include "heap.h"
#include "kernel.h"

/*
 * Priority search tree implementation.
 * The tree is a BST keyed by (prio, index) with the invariant that
 * the root of each subtree has the highest priority among its nodes.
 * This is essentially a treap (Cartesian tree) using explicit priorities.
 */

void prio_tree_init(struct prio_tree_root *root)
{
    root->prio_tree_node = NULL;
    root->count = 0;
}

/* Rotate right */
static void rotate_right(struct prio_tree_root *root,
                          struct prio_tree_node **parent_link,
                          struct prio_tree_node *node)
{
    (void)root;
    struct prio_tree_node *left = node->left;
    node->left = left->right;
    if (left->right)
        left->right->parent = node;
    left->right = node;
    left->parent = node->parent;
    node->parent = left;
    *parent_link = left;
}

/* Rotate left */
static void rotate_left(struct prio_tree_root *root,
                         struct prio_tree_node **parent_link,
                         struct prio_tree_node *node)
{
    (void)root;
    struct prio_tree_node *right = node->right;
    node->right = right->left;
    if (right->left)
        right->left->parent = node;
    right->left = node;
    right->parent = node->parent;
    node->parent = right;
    *parent_link = right;
}

int prio_tree_insert(struct prio_tree_root *root,
                     struct prio_tree_node *node)
{
    struct prio_tree_node *curr = root->prio_tree_node;
    struct prio_tree_node *parent = NULL;
    int is_left = 0;

    /* Standard BST insert by index */
    while (curr) {
        parent = curr;
        if (node->index < curr->index) {
            curr = curr->left;
            is_left = 1;
        } else if (node->index > curr->index) {
            curr = curr->right;
            is_left = 0;
        } else {
            /* Duplicate index – not allowed */
            return -1;
        }
    }

    node->left = node->right = NULL;
    node->parent = parent;

    if (!parent) {
        root->prio_tree_node = node;
    } else if (is_left) {
        parent->left = node;
    } else {
        parent->right = node;
    }

    /* Bubble up to maintain heap-order by priority */
    while (node->parent && node->prio > node->parent->prio) {
        struct prio_tree_node *par = node->parent;
        struct prio_tree_node **link;

        if (!par->parent) {
            link = &root->prio_tree_node;
        } else if (par == par->parent->left) {
            link = &par->parent->left;
        } else {
            link = &par->parent->right;
        }

        if (node == par->left)
            rotate_right(root, link, par);
        else
            rotate_left(root, link, par);
    }

    root->count++;
    return 0;
}

/* Remove a node from the priority search tree. */
void prio_tree_remove(struct prio_tree_root *root,
                      struct prio_tree_node *node)
{
    /* Rotate down until it becomes a leaf */
    while (node->left || node->right) {
        struct prio_tree_node **link;

        if (!node->parent) {
            link = &root->prio_tree_node;
        } else if (node == node->parent->left) {
            link = &node->parent->left;
        } else {
            link = &node->parent->right;
        }

        if (!node->right ||
            (node->left && node->left->prio > node->right->prio)) {
            rotate_right(root, link, node);
        } else {
            rotate_left(root, link, node);
        }
    }

    /* Now node is a leaf */
    if (!node->parent) {
        root->prio_tree_node = NULL;
    } else if (node == node->parent->left) {
        node->parent->left = NULL;
    } else {
        node->parent->right = NULL;
    }

    node->left = node->right = node->parent = NULL;
    root->count--;
}

/* Find the highest-priority node in [min_index, max_index]. */
struct prio_tree_node *prio_tree_search(struct prio_tree_root *root,
                                        uint64_t min_index,
                                        uint64_t max_index)
{
    if (!root->prio_tree_node)
        return NULL;

    /* BFS-style search: try the root first (highest priority overall) */
    struct prio_tree_node *best = NULL;

    /* Simple in-order traversal checking all nodes */
    /* For efficiency, we could prune, but this is a straightforward impl. */
    struct prio_tree_node *stack[64];
    int sp = 0;
    struct prio_tree_node *curr = root->prio_tree_node;

    while (sp > 0 || curr) {
        while (curr) {
            stack[sp++] = curr;
            curr = curr->left;
        }
        curr = stack[--sp];
        if (curr->index >= min_index && curr->index <= max_index) {
            if (!best || curr->prio > best->prio)
                best = curr;
        }
        curr = curr->right;
    }

    return best;
}

void prio_tree_iterate(struct prio_tree_root *root,
                       void (*fn)(struct prio_tree_node *, void *),
                       void *ctx)
{
    /* In-order traversal (sorted by index) */
    struct prio_tree_node *stack[64];
    int sp = 0;
    struct prio_tree_node *curr = root->prio_tree_node;

    while (sp > 0 || curr) {
        while (curr) {
            stack[sp++] = curr;
            curr = curr->left;
        }
        curr = stack[--sp];
        fn(curr, ctx);
        curr = curr->right;
    }
}

void prio_tree_init_global(void)
{
    kprintf("[OK] prio_tree: Priority search tree initialised\n");
}
