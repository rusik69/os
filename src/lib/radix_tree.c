#include "radix_tree.h"
#include "heap.h"
#include "string.h"
void radix_tree_init(struct radix_tree_root *root) { root->height = 0; root->rnode = NULL; }
int radix_tree_insert(struct radix_tree_root *root, unsigned long key, void *item) {
    if (!root->rnode) { root->rnode = item; root->height = 0; return 0; }
    if (root->height == 0) {
        struct radix_tree_node *n = kmalloc(sizeof(struct radix_tree_node));
        if (!n) return -1;
        memset(n, 0, sizeof(*n));
        n->slots[0] = root->rnode; n->count = 1;
        root->rnode = n; root->height = 1;
    }
    struct radix_tree_node *cur = root->rnode;
    int shift = (root->height - 1) * RADIX_TREE_MAP_SHIFT;
    for (int h = root->height; h > 0; h--) {
        int idx = (key >> shift) & (RADIX_TREE_MAP_SIZE - 1);
        if (h == 1) { cur->slots[idx] = item; cur->count++; return 0; }
        if (!cur->slots[idx]) {
            struct radix_tree_node *n = kmalloc(sizeof(struct radix_tree_node));
            if (!n) return -1;
            memset(n, 0, sizeof(*n));
            cur->slots[idx] = n; cur->count++;
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
void *radix_tree_delete(struct radix_tree_root *root, unsigned long key) { (void)root; (void)key; return NULL; }

/* ── Stub: radix_tree_delete ─────────────────────────────── */
void* radix_tree_delete(void *root, unsigned long index)
{
    (void)root;
    (void)index;
    kprintf("[radix_tree] radix_tree_delete: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: radix_tree_lookup ─────────────────────────────── */
void* radix_tree_lookup(void *root, unsigned long index)
{
    (void)root;
    (void)index;
    kprintf("[radix_tree] radix_tree_lookup: not yet implemented\n");
    return -ENOSYS;
}
