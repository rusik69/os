#ifndef HASHTABLE_H
#define HASHTABLE_H

#include "types.h"
#include "list.h"

#define HASH_TABLE_SIZE 64

struct hashtable_node {
    struct list_head list;
    uint64_t         key;
    void            *value;
};

struct hashtable {
    struct list_head buckets[HASH_TABLE_SIZE];
    int              count;
};

/* Initialise a hash table (all buckets empty). */
void hashtable_init(struct hashtable *ht);

/* Insert key/value. Returns 0 on success, -ENOMEM on allocation failure. */
int hashtable_insert(struct hashtable *ht, uint64_t key, void *value);

/* Look up a key. Returns the value, or NULL if not found. */
void *hashtable_lookup(struct hashtable *ht, uint64_t key);

/* Remove a key. Returns 0 if found and removed, -ENOENT otherwise. */
int hashtable_remove(struct hashtable *ht, uint64_t key);

/* Iterate: call fn(key, value, ctx) for each entry. */
void hashtable_iterate(struct hashtable *ht,
                       void (*fn)(uint64_t key, void *value, void *ctx),
                       void *ctx);

/* Return the number of entries. */
static inline int hashtable_count(struct hashtable *ht) { return ht->count; }

void hashtable_init_global(void);

#endif /* HASHTABLE_H */
