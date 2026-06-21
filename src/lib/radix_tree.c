#include "radix_tree.h"
#include "heap.h"
#include "string.h"

void radix_tree_init(struct radix_tree_root *root) {
    root->height = 0;
    root->rnode = NULL;
}

int radix_tree_insert(struct radix_tree_root *root, unsigned long key, void *item) {
    if (!root->rnode) {
        root->rnode = item;
        root->height = 0;
        return 0;
    }
    if (root->height == 0) {
        struct radix_tree_node *n = kmalloc(sizeof(struct radix_tree_node));
        if (!n) return -1;
        memset(n, 0, sizeof(*n));
        n->slots[0] = root->rnode;
        n->count = 1;
        root->rnode = n;
        root->height = 1;
    }
    struct radix_tree_node *cur = root->rnode;
    int shift = (root->height - 1) * RADIX_TREE_MAP_SHIFT;
    for (int h = root->height; h > 0; h--) {
        int idx = (key >> shift) & (RADIX_TREE_MAP_SIZE - 1);
        if (h == 1) {
            if (!cur->slots[idx]) cur->count++;
            cur->slots[idx] = item;
            return 0;
        }
        if (!cur->slots[idx]) {
            struct radix_tree_node *n = kmalloc(sizeof(struct radix_tree_node));
            if (!n) return -1;
            memset(n, 0, sizeof(*n));
            cur->slots[idx] = n;
            cur->count++;
        }
        cur = cur->slots[idx];
        shift -= RADIX_TREE_MAP_SHIFT;
    }
    return 0;
}

void *radix_tree_lookup(struct radix_tree_root *root, unsigned long key) {
    if (!root->rnode) return NULL;
    if (root->height == 0) return (key == 0) ? root->rnode : NULL;
    struct radix_tree_node *cur = root->rnode;
    int shift = (root->height - 1) * RADIX_TREE_MAP_SHIFT;
    for (int h = root->height; h > 0; h--) {
        int idx = (key >> shift) & (RADIX_TREE_MAP_SIZE - 1);
        if (h == 1) return cur->slots[idx];
        if (!cur->slots[idx]) return NULL;
        cur = cur->slots[idx];
        shift -= RADIX_TREE_MAP_SHIFT;
    }
    return NULL;
}

void *radix_tree_delete(struct radix_tree_root *root, unsigned long key) {
    if (!root->rnode) return NULL;

    if (root->height == 0) {
        if (key == 0) {
            void *item = root->rnode;
            root->rnode = NULL;
            return item;
        }
        return NULL;
    }

    /* Traverse and find the item, freeing empty nodes on the way back */
    struct radix_tree_node *cur = root->rnode;
    struct radix_tree_node *parent = NULL;
    int parent_idx = 0;
    int shift = (root->height - 1) * RADIX_TREE_MAP_SHIFT;

    for (int h = root->height; h > 0; h--) {
        int idx = (key >> shift) & (RADIX_TREE_MAP_SIZE - 1);
        if (h == 1) {
            if (!cur->slots[idx]) return NULL;
            void *item = cur->slots[idx];
            cur->slots[idx] = NULL;
            cur->count--;

            /* Clean up empty nodes */
            if (cur->count == 0 && parent && root->height > 1) {
                parent->slots[parent_idx] = NULL;
                parent->count--;
                kfree(cur);
                /* Propagate cleanup upward */
                struct radix_tree_node *p = parent;
                struct radix_tree_node *pp = NULL;
                struct radix_tree_node *c = cur;
                (void)pp; (void)c;
                int cleanup = 1;
                while (cleanup && p && root->height > 1) {
                    if (p->count == 0) {
                        /* Find p's parent */
                        /* Simple: just shrink tree if root has no children */
                        if (p == root->rnode) {
                            /* Root node has no children - collapse */
                            root->rnode = NULL;
                            root->height = 0;
                            kfree(p);
                        }
                    }
                    cleanup = 0;
                }
            }
            return item;
        }
        if (!cur->slots[idx]) return NULL;
        parent = cur;
        parent_idx = idx;
        cur = cur->slots[idx];
        shift -= RADIX_TREE_MAP_SHIFT;
    }
    return NULL;
}
