/*
 * test_hashtable.c — Host-side tests for kernel hash table
 *
 * Tests hashtable_init, hashtable_insert, hashtable_lookup,
 * hashtable_remove, hashtable_iterate from src/kernel/hashtable.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel type declarations (mirror kernel hashtable.h + list.h)
 * =================================================================== */
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(name) { &(name), &(name) }
static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list; list->prev = list;
}
static inline void __list_add(struct list_head *new, struct list_head *prev, struct list_head *next) {
    next->prev = new; new->next = next;
    new->prev = prev; prev->next = new;
}
static inline void list_add(struct list_head *new, struct list_head *head) {
    __list_add(new, head, head->next);
}
static inline void list_del(struct list_head *entry) {
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
}
#define list_entry(ptr, type, member) ((type *)((char *)(ptr) - (unsigned long)&((type *)0)->member))
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

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

extern void hashtable_init(struct hashtable *ht);
extern int hashtable_insert(struct hashtable *ht, uint64_t key, void *value);
extern void *hashtable_lookup(struct hashtable *ht, uint64_t key);
extern int hashtable_remove(struct hashtable *ht, uint64_t key);
extern void hashtable_iterate(struct hashtable *ht,
                              void (*fn)(uint64_t key, void *value, void *ctx),
                              void *ctx);

/* hashtable_count is static inline in kernel header — provide definition */
static inline int hashtable_count(struct hashtable *ht) { return ht->count; }

/* ===================================================================
 *  Stubs (stubs.o provides kmalloc/kfree)
 * =================================================================== */
void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* ===================================================================
 *  Test harness
 * =================================================================== */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do {                                           \
    if (!(cond)) {                                                      \
        printf("  FAIL: %s (%s)\n", name, #cond);                      \
        tests_failed++;                                                 \
    } else {                                                            \
        printf("  PASS: %s\n", name);                                   \
        tests_passed++;                                                 \
    }                                                                   \
} while (0)

/* Iteration context */
static int iter_keys[256];
static int iter_count;

static void iter_fn(uint64_t key, void *value, void *ctx) {
    (void)value; (void)ctx;
    iter_keys[iter_count++] = (int)key;
}

/* ===================================================================
 *  test_hashtable
 * =================================================================== */
static void test_hashtable(void)
{
    struct hashtable ht;

    /* 1. Init empty */
    hashtable_init(&ht);
    TEST("hashtable_init: count=0", hashtable_count(&ht) == 0);

    /* 2. Lookup on empty returns NULL */
    TEST("hashtable lookup: empty returns NULL",
         hashtable_lookup(&ht, 42) == NULL);

    /* 3. Insert */
    int val1 = 100;
    int r = hashtable_insert(&ht, 1, &val1);
    TEST("hashtable_insert: returns 0", r == 0);
    TEST("hashtable_insert: count=1", hashtable_count(&ht) == 1);

    /* 4. Lookup existing */
    int *vp = (int *)hashtable_lookup(&ht, 1);
    TEST("hashtable_lookup: finds key 1", vp != NULL && *vp == 100);

    /* 5. Lookup non-existing */
    TEST("hashtable_lookup: key 999 not found",
         hashtable_lookup(&ht, 999) == NULL);

    /* 6. Multiple inserts */
    int val2 = 200, val3 = 300, val4 = 400;
    hashtable_insert(&ht, 2, &val2);
    hashtable_insert(&ht, 3, &val3);
    hashtable_insert(&ht, 4, &val4);
    TEST("hashtable_insert: count=4", hashtable_count(&ht) == 4);

    /* 7. Lookup all */
    TEST("hashtable_lookup: key 2", *(int*)hashtable_lookup(&ht, 2) == 200);
    TEST("hashtable_lookup: key 3", *(int*)hashtable_lookup(&ht, 3) == 300);
    TEST("hashtable_lookup: key 4", *(int*)hashtable_lookup(&ht, 4) == 400);

    /* 8. Update existing (re-insert same key) */
    int val5 = 500;
    hashtable_insert(&ht, 2, &val5);
    TEST("hashtable_insert: update key 2", *(int*)hashtable_lookup(&ht, 2) == 500);
    TEST("hashtable_insert: count still 4", hashtable_count(&ht) == 4);

    /* 9. Remove */
    r = hashtable_remove(&ht, 3);
    TEST("hashtable_remove: returns 0", r == 0);
    TEST("hashtable_remove: count=3", hashtable_count(&ht) == 3);

    /* 10. Remove non-existing */
    r = hashtable_remove(&ht, 999);
    TEST("hashtable_remove: non-existent returns -1", r != 0);

    /* 11. Lookup after remove */
    TEST("hashtable_lookup: removed key returns NULL",
         hashtable_lookup(&ht, 3) == NULL);

    /* 12. Iterate */
    iter_count = 0;
    hashtable_iterate(&ht, iter_fn, NULL);
    TEST("hashtable_iterate: visits 3 items", iter_count == 3);

    /* Check all expected keys present */
    int found[4] = {0};
    for (int i = 0; i < iter_count; i++) {
        if (iter_keys[i] == 1) found[0] = 1;
        if (iter_keys[i] == 2) found[1] = 1;
        if (iter_keys[i] == 4) found[2] = 1;
        if (iter_keys[i] == 3) found[3] = 1;
    }
    TEST("hashtable_iterate: key 1 present", found[0]);
    TEST("hashtable_iterate: key 2 present", found[1]);
    TEST("hashtable_iterate: key 4 present", found[2]);
    TEST("hashtable_iterate: key 3 (removed) absent", !found[3]);

    /* 13. Large key values (edge case for hashing) */
    int val6 = 600;
    hashtable_insert(&ht, 0xFFFFFFFFFFFFFFFFULL, &val6);
    TEST("hashtable_lookup: max key",
         *(int*)hashtable_lookup(&ht, 0xFFFFFFFFFFFFFFFFULL) == 600);

    /* 14. Many inserts to test bucket distribution */
    for (int i = 0; i < 50; i++) {
        int *v = (int *)malloc(sizeof(int));
        *v = i;
        hashtable_insert(&ht, i + 1000, v);
    }
    TEST("hashtable: 50 inserts successful", hashtable_count(&ht) >= 50);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Hash Table Tests ===\n\n");
    test_hashtable();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
