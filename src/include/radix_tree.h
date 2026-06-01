#ifndef RADIX_TREE_H
#define RADIX_TREE_H
#include "types.h"
#define RADIX_TREE_MAP_SHIFT 6
#define RADIX_TREE_MAP_SIZE (1UL << RADIX_TREE_MAP_SHIFT)
struct radix_tree_root { unsigned int height; void *rnode; };
struct radix_tree_node {
    unsigned int height;
    void *slots[64];
    unsigned int count;
};
void radix_tree_init(struct radix_tree_root *root);
int radix_tree_insert(struct radix_tree_root *root, unsigned long key, void *item);
void *radix_tree_lookup(struct radix_tree_root *root, unsigned long key);
void *radix_tree_delete(struct radix_tree_root *root, unsigned long key);
#endif
