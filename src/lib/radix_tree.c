/*
 * radix_tree.c — Radix tree implementation
 *
 * A radix tree (aka Patricia trie) is a space-optimized trie data structure
 * used to map unsigned long keys to void* values. It is commonly used in OS
 * kernels for page cache tracking, IDR (ID radix tree) allocation, and other
 * sparse-address mappings.
 *
 * Key parameters:
 *   RADIX_TREE_MAP_SHIFT = 6  → each node has 64 slots (2^6)
 *   RADIX_TREE_MAP_SIZE   = 64
 *
 * Addressing scheme:
 *   The key is treated as a bit string split into chunks of 6 bits starting
 *   from the MSB. Each level of the tree consumes one 6-bit chunk. A tree of
 *   height H can therefore address keys with up to H*6 significant bits.
 *
 *   For height == 0: only key 0 is stored (single leaf directly in root->rnode).
 *   For height == 1: root->rnode points to a struct radix_tree_node with 64
 *     slots. The bottom 6 bits of the key index into slots[].
 *   For height H: the topmost chunk indexes into the root node, and each
 *     internal node routes to the next level via the next 6-bit chunk.
 *
 * Memory model:
 *   - Root nodes (struct radix_tree_root) are statically embedded.
 *   - Internal nodes (struct radix_tree_node) are heap-allocated via kmalloc.
 *   - Leaf items are stored in the slot array of the leaf-level node.
 *   - Empty nodes are freed during deletion when their count drops to 0.
 */

#include "radix_tree.h"
#include "heap.h"
#include "string.h"

/*
 * radix_tree_init — Initialise an empty radix tree root.
 * @root: pointer to the tree root (must not be NULL).
 *
 * Sets height to 0 and rnode to NULL, indicating an empty tree.
 */
void radix_tree_init(struct radix_tree_root *root) {
    root->height = 0;
    root->rnode = NULL;
}

/*
 * radix_tree_insert — Insert a value into the radix tree.
 * @root: the tree root (must be initialised with radix_tree_init()).
 * @key:  the unsigned integer lookup key.
 * @item: opaque pointer to store (must not be NULL — NULL means empty slot).
 *
 * Returns 0 on success, or -1 if memory allocation fails.
 *
 * Algorithm overview:
 *   1. Empty tree  (rnode == NULL): store @item directly as the sole leaf.
 *   2. Single leaf (height == 0):  allocate a level-1 node, move the existing
 *      leaf into slot[0], then insert @item into the appropriate leaf slot.
 *   3. Multi-level (height >= 1):  walk the tree from the root, using 6-bit
 *      chunks of the key to index into each node's slots[].  Allocate missing
 *      internal nodes on demand.  At the leaf level (h == 1) place @item in
 *      the slot addressed by the bottom 6 bits.
 */
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

/*
 * radix_tree_lookup — Retrieve the value stored at @key.
 * @root: the tree root.
 * @key:  the lookup key.
 *
 * Returns the stored pointer, or NULL if no item exists at @key.
 *
 * Traversal:
 *   - height == 0:  only key 0 is valid; return root->rnode or NULL.
 *   - height >= 1:  walk the tree, extracting 6-bit index chunks from the key
 *     at each level.  At the leaf level (h == 1) return slots[idx]; return
 *     NULL early if any intermediate slot is empty.
 */
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

/*
 * radix_tree_delete — Remove and return the value at @key.
 * @root: the tree root.
 * @key:  the lookup key.
 *
 * Returns the removed pointer, or NULL if no item existed at @key.
 *
 * Algorithm:
 *   1. Empty tree (rnode == NULL): return NULL immediately.
 *   2. Single leaf (height == 0):  return the leaf if key == 0, else NULL.
 *      Set rnode = NULL afterwards.
 *   3. Multi-level (height >= 1):  traverse to the leaf level, NULL the slot,
 *      decrement the node's count, and walk back up freeing any nodes whose
 *      count drops to 0.  If the entire root node becomes empty, collapse the
 *      tree back to height == 0.
 */
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
