/*
 * test_list.c — Host-side unit tests for kernel linked list and hash table algorithms
 *
 * Tests core doubly-linked list operations (init, add, del, entry)
 * and a simple hash table implementation using chaining.
 *
 * Runs entirely on the host — no kernel dependencies.
 *
 * Compile:  gcc -Wall -Werror -g -O0 -o test_list test_list.c
 * Run:      ./test_list
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================
 *  Kernel-compatible doubly-linked list
 *
 *  Mirrors linux/list.h: struct list_head with container_of/list_entry,
 *  list_add, list_del, list_for_each, etc.
 * =================================================================== */

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
    struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *new,
                              struct list_head *prev,
                              struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

static inline void list_del_init(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static inline int list_is_singular(const struct list_head *head)
{
    return !list_empty(head) && (head->next == head->prev);
}

static inline int list_is_last(const struct list_head *list,
                                const struct list_head *head)
{
    return list->next == head;
}

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

#define list_last_entry(ptr, type, member) \
    list_entry((ptr)->prev, type, member)

#define list_next_entry(pos, member) \
    list_entry((pos)->member.next, typeof(*(pos)), member)

#define list_prev_entry(pos, member) \
    list_entry((pos)->member.prev, typeof(*(pos)), member)

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
         pos = n, n = pos->next)

#define list_for_each_entry(pos, head, member) \
    for (pos = list_entry((head)->next, typeof(*pos), member); \
         &pos->member != (head); \
         pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_entry((head)->next, typeof(*pos), member), \
         n = list_entry(pos->member.next, typeof(*pos), member); \
         &pos->member != (head); \
         pos = n, n = list_entry(n->member.next, typeof(*n), member))

/* Container struct for list operations testing */
struct list_item {
    int id;
    struct list_head list;
};

/* ===================================================================
 *  Simple hash table using chaining
 *
 *  Fixed-size bucket array, each bucket is a list_head.
 * =================================================================== */

#define HASH_TABLE_SIZE 32

struct hash_table {
    struct list_head buckets[HASH_TABLE_SIZE];
};

#define hash_fn(key) ((uint32_t)(key) % HASH_TABLE_SIZE)

struct hash_entry {
    uint32_t key;
    uint32_t value;
    struct list_head list;
};

static void hash_table_init(struct hash_table *ht)
{
    for (int i = 0; i < HASH_TABLE_SIZE; i++)
        INIT_LIST_HEAD(&ht->buckets[i]);
}

static void hash_table_insert(struct hash_table *ht, uint32_t key, uint32_t value)
{
    struct hash_entry *entry = (struct hash_entry *)malloc(sizeof(*entry));
    if (!entry) return;
    entry->key = key;
    entry->value = value;
    uint32_t bucket = hash_fn(key);
    list_add_tail(&entry->list, &ht->buckets[bucket]);
}

static struct hash_entry *hash_table_lookup(struct hash_table *ht, uint32_t key)
{
    uint32_t bucket = hash_fn(key);
    struct hash_entry *pos;
    list_for_each_entry(pos, &ht->buckets[bucket], list) {
        if (pos->key == key)
            return pos;
    }
    return NULL;
}

static int hash_table_remove(struct hash_table *ht, uint32_t key)
{
    uint32_t bucket = hash_fn(key);
    struct hash_entry *pos, *n;
    list_for_each_entry_safe(pos, n, &ht->buckets[bucket], list) {
        if (pos->key == key) {
            list_del(&pos->list);
            free(pos);
            return 1;
        }
    }
    return 0;
}

static void hash_table_destroy(struct hash_table *ht)
{
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        struct hash_entry *pos, *n;
        list_for_each_entry_safe(pos, n, &ht->buckets[i], list) {
            list_del(&pos->list);
            free(pos);
        }
    }
}

/* ===================================================================
 *  Test framework
 * =================================================================== */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)  do {                        \
    tests_run++;                                \
    printf("  TEST: %-50s ... ", name);         \
} while (0)

#define PASS()      do {                        \
    tests_passed++;                             \
    printf("PASS\n");                           \
} while (0)

#define FAIL(msg)   do {                        \
    tests_failed++;                             \
    printf("FAIL\n");                           \
    printf("        %s\n", msg);                \
} while (0)

#define ASSERT(cond, msg) do {                  \
    if (!(cond)) { FAIL(msg); return; }         \
} while (0)

#define ASSERT_INT_EQ(got, expected, msg) do {  \
    if ((got) != (expected)) {                  \
        tests_failed++;                         \
        printf("FAIL\n");                       \
        printf("        %s\n", msg);            \
        printf("        Expected: %d\n",        \
               (int)(expected));                \
        printf("        Got:      %d\n",        \
               (int)(got));                     \
        return;                                 \
    }                                           \
} while (0)

#define ASSERT_PTR_NONNULL(ptr, msg) do {       \
    if ((ptr) == NULL) {                        \
        tests_failed++;                         \
        printf("FAIL\n");                       \
        printf("        %s\n", msg);            \
        printf("        Expected non-NULL\n");  \
        return;                                 \
    }                                           \
} while (0)

/* ===================================================================
 *  Tests: Linked list
 * =================================================================== */

static void test_list_init_empty(void)
{
    LIST_HEAD(head);

    TEST("initialized list is empty");
    ASSERT(list_empty(&head), "list should be empty");
    PASS();
}

static void test_list_add_single(void)
{
    LIST_HEAD(head);
    struct list_head node;
    INIT_LIST_HEAD(&node);

    TEST("add one node, list not empty");
    list_add(&node, &head);
    ASSERT(!list_empty(&head), "list should not be empty");
    PASS();

    TEST("added node is first entry");
    ASSERT(list_first_entry(&head, struct list_head, next) == &node,
           "first entry should be the added node");
    PASS();

    TEST("added node is last entry (singular)");
    ASSERT(list_is_singular(&head), "list should have exactly one element");
    PASS();
}

static void test_list_add_tail(void)
{
    struct list_item a, b;
    LIST_HEAD(head);
    INIT_LIST_HEAD(&a.list);
    INIT_LIST_HEAD(&b.list);

    list_add_tail(&a.list, &head);
    list_add_tail(&b.list, &head);

    TEST("add_tail preserves insertion order");
    ASSERT(list_first_entry(&head, struct list_item, list) == &a,
           "first entry should be 'a'");
    ASSERT(list_last_entry(&head, struct list_item, list) == &b,
           "last entry should be 'b'");
    PASS();
}

static void test_list_add_head(void)
{
    struct list_item a, b;
    LIST_HEAD(head);
    INIT_LIST_HEAD(&a.list);
    INIT_LIST_HEAD(&b.list);

    list_add(&a.list, &head);   /* a first */
    list_add(&b.list, &head);   /* b becomes first (LIFO) */

    TEST("add inserts at head (LIFO order)");
    ASSERT(list_first_entry(&head, struct list_item, list) == &b,
           "first entry should be 'b' (most recently added)");
    ASSERT(list_last_entry(&head, struct list_item, list) == &a,
           "last entry should be 'a'");
    PASS();
}

static void test_list_del(void)
{
    struct list_item a, b, c;
    LIST_HEAD(head);
    INIT_LIST_HEAD(&a.list);
    INIT_LIST_HEAD(&b.list);
    INIT_LIST_HEAD(&c.list);

    list_add_tail(&a.list, &head);
    list_add_tail(&b.list, &head);
    list_add_tail(&c.list, &head);

    TEST("delete middle node");
    list_del(&b.list);
    ASSERT(!list_empty(&head), "list should not be empty after deleting middle");
    ASSERT(list_first_entry(&head, struct list_item, list) == &a,
           "first should be 'a'");
    ASSERT(list_last_entry(&head, struct list_item, list) == &c,
           "last should be 'c'");
    PASS();

    TEST("delete first node");
    list_del(&a.list);
    ASSERT(list_first_entry(&head, struct list_item, list) == &c,
           "first should be 'c' after deleting 'a'");
    PASS();

    TEST("delete last node, list becomes empty");
    list_del(&c.list);
    ASSERT(list_empty(&head), "list should be empty");
    PASS();
}

static void test_list_del_init(void)
{
    struct list_item node;
    LIST_HEAD(head);
    INIT_LIST_HEAD(&node.list);

    list_add(&node.list, &head);
    list_del_init(&node.list);

    TEST("list_del_init restores node to empty state");
    ASSERT(list_empty(&head), "head should be empty after del_init");
    ASSERT(node.list.next == &node.list, "node.next should point to itself");
    ASSERT(node.list.prev == &node.list, "node.prev should point to itself");
    PASS();
}

static void test_list_for_each(void)
{
    struct list_item nodes[5];
    LIST_HEAD(head);
    int count = 0;

    for (int i = 0; i < 5; i++) {
        INIT_LIST_HEAD(&nodes[i].list);
        list_add_tail(&nodes[i].list, &head);
    }

    TEST("list_for_each iterates all 5 nodes");
    struct list_head *pos;
    list_for_each(pos, &head) {
        count++;
    }
    ASSERT_INT_EQ(count, 5, "should have iterated 5 nodes");
    PASS();
}

static void test_list_for_each_safe(void)
{
    struct list_item nodes[5];
    LIST_HEAD(head);
    int count = 0;

    for (int i = 0; i < 5; i++) {
        INIT_LIST_HEAD(&nodes[i].list);
        list_add_tail(&nodes[i].list, &head);
    }

    TEST("list_for_each_safe can delete while iterating");
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &head) {
        list_del(pos);
        count++;
    }
    ASSERT_INT_EQ(count, 5, "should have deleted 5 nodes");
    ASSERT(list_empty(&head), "list should be empty after deleting all");
    PASS();
}

/* ===================================================================
 *  Tests: container_of and list_entry
 * =================================================================== */

struct test_item {
    int id;
    char name[16];
    struct list_head list;
};

static void test_container_of(void)
{
    struct test_item item;
    item.id = 42;

    TEST("container_of recovers struct from member pointer");
    struct test_item *ptr = container_of(&item.list, struct test_item, list);
    ASSERT(ptr == &item, "container_of should return original struct");
    ASSERT_INT_EQ(ptr->id, 42, "member value should be correct");
    PASS();
}

static void test_list_entry(void)
{
    LIST_HEAD(head);
    struct test_item item;
    item.id = 99;
    INIT_LIST_HEAD(&item.list);
    list_add_tail(&item.list, &head);

    TEST("list_entry recovers struct from list node");
    struct test_item *entry = list_first_entry(&head, struct test_item, list);
    ASSERT(entry == &item, "list_entry should return original struct");
    ASSERT_INT_EQ(entry->id, 99, "member value should be correct");
    PASS();
}

static void test_list_for_each_entry(void)
{
    LIST_HEAD(head);
    struct test_item items[4];
    int sum = 0;

    for (int i = 0; i < 4; i++) {
        items[i].id = i * 10;
        INIT_LIST_HEAD(&items[i].list);
        list_add_tail(&items[i].list, &head);
    }

    TEST("list_for_each_entry iterates container structs");
    struct test_item *pos;
    list_for_each_entry(pos, &head, list) {
        sum += pos->id;
    }
    ASSERT_INT_EQ(sum, 0 + 10 + 20 + 30, "sum of ids should be 60");
    PASS();
}

/* ===================================================================
 *  Tests: Hash table
 * =================================================================== */

static void test_hash_insert_lookup(void)
{
    struct hash_table ht;
    hash_table_init(&ht);

    TEST("hash_insert followed by lookup");
    hash_table_insert(&ht, 42, 100);
    struct hash_entry *e = hash_table_lookup(&ht, 42);
    ASSERT_PTR_NONNULL(e, "lookup of inserted key");
    ASSERT_INT_EQ(e->key, 42, "key match");
    ASSERT_INT_EQ(e->value, 100, "value match");
    PASS();

    hash_table_destroy(&ht);
}

static void test_hash_lookup_missing(void)
{
    struct hash_table ht;
    hash_table_init(&ht);

    TEST("hash lookup of missing key returns NULL");
    struct hash_entry *e = hash_table_lookup(&ht, 999);
    ASSERT(e == NULL, "lookup of missing key should return NULL");
    PASS();

    hash_table_destroy(&ht);
}

static void test_hash_insert_multiple(void)
{
    struct hash_table ht;
    hash_table_init(&ht);

    TEST("hash table handles multiple entries");
    for (int i = 0; i < 100; i++) {
        hash_table_insert(&ht, (uint32_t)i, (uint32_t)(i * 2));
    }
    for (int i = 0; i < 100; i++) {
        struct hash_entry *e = hash_table_lookup(&ht, (uint32_t)i);
        ASSERT_PTR_NONNULL(e, "lookup should succeed");
        ASSERT_INT_EQ(e->value, i * 2, "value should match");
    }
    PASS();

    hash_table_destroy(&ht);
}

static void test_hash_remove(void)
{
    struct hash_table ht;
    hash_table_init(&ht);

    hash_table_insert(&ht, 42, 100);
    hash_table_insert(&ht, 74, 200);

    TEST("hash_remove removes entry");
    int ret = hash_table_remove(&ht, 42);
    ASSERT_INT_EQ(ret, 1, "remove should succeed");

    struct hash_entry *e = hash_table_lookup(&ht, 42);
    ASSERT(e == NULL, "lookup after remove should fail");

    /* Other entry should still exist */
    e = hash_table_lookup(&ht, 74);
    ASSERT_PTR_NONNULL(e, "other entry should survive");
    ASSERT_INT_EQ(e->value, 200, "other entry value intact");
    PASS();

    hash_table_destroy(&ht);
}

static void test_hash_remove_missing(void)
{
    struct hash_table ht;
    hash_table_init(&ht);

    TEST("hash_remove of missing key returns 0");
    int ret = hash_table_remove(&ht, 999);
    ASSERT_INT_EQ(ret, 0, "remove of missing should return 0");
    PASS();

    hash_table_destroy(&ht);
}

static void test_hash_collision(void)
{
    struct hash_table ht;
    hash_table_init(&ht);

    /* Keys that hash to the same bucket (same modulo HASH_TABLE_SIZE) */
    uint32_t k1 = 0;
    uint32_t k2 = HASH_TABLE_SIZE;  /* same bucket as k1 */

    TEST("hash table handles collisions (same bucket)");
    hash_table_insert(&ht, k1, 10);
    hash_table_insert(&ht, k2, 20);

    struct hash_entry *e1 = hash_table_lookup(&ht, k1);
    struct hash_entry *e2 = hash_table_lookup(&ht, k2);
    ASSERT_PTR_NONNULL(e1, "lookup k1");
    ASSERT_PTR_NONNULL(e2, "lookup k2");
    ASSERT_INT_EQ(e1->value, 10, "k1 value");
    ASSERT_INT_EQ(e2->value, 20, "k2 value");

    /* Delete k1, verify k2 still exists */
    hash_table_remove(&ht, k1);
    e2 = hash_table_lookup(&ht, k2);
    ASSERT_PTR_NONNULL(e2, "k2 should survive after k1 removed");
    ASSERT_INT_EQ(e2->value, 20, "k2 value intact");
    PASS();

    hash_table_destroy(&ht);
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("============================================\n");
    printf("  Linked List & Hash Table Unit Tests\n");
    printf("============================================\n\n");

    /* Linked list tests */
    test_list_init_empty();
    test_list_add_single();
    test_list_add_tail();
    test_list_add_head();
    test_list_del();
    test_list_del_init();
    test_list_for_each();
    test_list_for_each_safe();

    /* container_of / list_entry tests */
    test_container_of();
    test_list_entry();
    test_list_for_each_entry();

    /* Hash table tests */
    test_hash_insert_lookup();
    test_hash_lookup_missing();
    test_hash_insert_multiple();
    test_hash_remove();
    test_hash_remove_missing();
    test_hash_collision();

    printf("\n============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
