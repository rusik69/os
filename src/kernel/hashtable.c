#include "hashtable.h"
#include "printf.h"
#include "heap.h"
#include "string.h"
#include "errno.h"
#include "kernel.h"

static inline uint32_t hash_key(uint64_t key)
{
    /* Thomas Wang integer hash */
    uint64_t h = key;
    h = ~h + (h << 15);
    h ^= (h >> 12);
    h += (h << 2);
    h ^= (h >> 4);
    h *= 2057;
    h ^= (h >> 16);
    return (uint32_t)(h & (HASH_TABLE_SIZE - 1));
}

void hashtable_init(struct hashtable *ht)
{
    int i;
    for (i = 0; i < HASH_TABLE_SIZE; i++)
        INIT_LIST_HEAD(&ht->buckets[i]);
    ht->count = 0;
}

int hashtable_insert(struct hashtable *ht, uint64_t key, void *value)
{
    struct hashtable_node *node;
    uint32_t idx = hash_key(key);

    /* Check for duplicate key */
    struct list_head *pos;
    list_for_each(pos, &ht->buckets[idx]) {
        node = list_entry(pos, struct hashtable_node, list);
        if (node->key == key) {
            node->value = value;  /* update existing */
            return 0;
        }
    }

    node = (struct hashtable_node *)kmalloc(sizeof(*node));
    if (!node)
        return -ENOMEM;

    node->key   = key;
    node->value = value;
    list_add(&node->list, &ht->buckets[idx]);
    ht->count++;
    return 0;
}

void *hashtable_lookup(struct hashtable *ht, uint64_t key)
{
    uint32_t idx = hash_key(key);
    struct list_head *pos;

    list_for_each(pos, &ht->buckets[idx]) {
        struct hashtable_node *node = list_entry(pos, struct hashtable_node, list);
        if (node->key == key)
            return node->value;
    }
    return NULL;
}

int hashtable_remove(struct hashtable *ht, uint64_t key)
{
    uint32_t idx = hash_key(key);
    struct list_head *pos, *n;

    list_for_each_safe(pos, n, &ht->buckets[idx]) {
        struct hashtable_node *node = list_entry(pos, struct hashtable_node, list);
        if (node->key == key) {
            list_del(&node->list);
            kfree(node);
            ht->count--;
            return 0;
        }
    }
    return -ENOENT;
}

void hashtable_iterate(struct hashtable *ht,
                       void (*fn)(uint64_t key, void *value, void *ctx),
                       void *ctx)
{
    int i;
    struct list_head *pos;

    for (i = 0; i < HASH_TABLE_SIZE; i++) {
        list_for_each(pos, &ht->buckets[i]) {
            struct hashtable_node *node = list_entry(pos, struct hashtable_node, list);
            fn(node->key, node->value, ctx);
        }
    }
}

void hashtable_init_global(void)
{
    kprintf("[OK] hashtable: Hash table initialised (size=%d)\n", HASH_TABLE_SIZE);
}

/* ── Stub: hashtable_lookup ─────────────────────────────── */
void* hashtable_lookup(void *ht, unsigned long key)
{
    (void)ht;
    (void)key;
    kprintf("[hashtable] hashtable_lookup: not yet implemented\n");
    return -ENOSYS;
}
