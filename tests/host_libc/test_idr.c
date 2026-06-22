/*
 * test_idr.c — Host-side tests for kernel IDR (Integer ID) allocator
 *
 * Tests idr_init, idr_alloc, idr_remove, idr_find from src/kernel/idr.c.
 * Uses kmalloc/__builtin_ctzll — both available on host.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel type declarations (mirror kernel types.h + idr.h)
 * =================================================================== */
struct idr {
    uint64_t *bitmap;
    int       max;
    int       nwords;
};

extern int idr_init(struct idr *idr, int max);
extern int idr_alloc(struct idr *idr);
extern void idr_remove(struct idr *idr, int id);
extern int idr_find(struct idr *idr, int id);
extern int idr_destroy(void *idr);

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

/* ===================================================================
 *  test_idr_init
 * =================================================================== */
static void test_idr_init(void)
{
    struct idr idr;

    /* 1. Normal init */
    int r = idr_init(&idr, 64);
    TEST("idr_init: returns 0 on success", r == 0);
    TEST("idr_init: max = 64", idr.max == 64);
    TEST("idr_init: bitmap non-NULL", idr.bitmap != NULL);
    idr_remove(&idr, 0); /* just to check it doesn't crash on valid init */

    /* 2. Zero max — should fail */
    r = idr_init(&idr, 0);
    TEST("idr_init: rejects max=0", r == -1);

    /* 3. Negative max — should fail */
    r = idr_init(&idr, -1);
    TEST("idr_init: rejects negative max", r == -1);

    /* 4. NULL idr */
    r = idr_init(NULL, 64);
    TEST("idr_init: NULL returns -1", r == -1);

    /* 5. Small max (1 — single ID) */
    r = idr_init(&idr, 1);
    TEST("idr_init: max=1 succeeds", r == 0);
    idr_remove(&idr, 0);
}

/* ===================================================================
 *  test_idr_alloc
 * =================================================================== */
static void test_idr_alloc(void)
{
    struct idr idr;
    idr_init(&idr, 64);

    /* 1. First allocation returns 0 */
    TEST("idr_alloc: first ID is 0", idr_alloc(&idr) == 0);

    /* 2. Second allocation returns 1 */
    TEST("idr_alloc: second ID is 1", idr_alloc(&idr) == 1);

    /* 3. Fill remaining */
    for (int i = 2; i < 64; i++) {
        int id = idr_alloc(&idr);
        if (id != i) { TEST("idr_alloc: sequential fill", 0); break; }
    }
    TEST("idr_alloc: sequential fill completes", idr_alloc(&idr) == -1);
    TEST("idr_alloc: full returns -1", 1);

    /* 4. Allocate from new IDR */
    struct idr idr2;
    idr_init(&idr2, 10);
    for (int i = 0; i < 10; i++) idr_alloc(&idr2);
    TEST("idr_alloc: 10-entries full", idr_alloc(&idr2) == -1);
}

/* ===================================================================
 *  test_idr_remove_find
 * =================================================================== */
static void test_idr_remove_find(void)
{
    struct idr idr;
    idr_init(&idr, 128);

    /* Allocate and verify find */
    int id5 = idr_alloc(&idr);  /* 0 */
    int id10 = idr_alloc(&idr); /* 1 */
    idr_alloc(&idr); idr_alloc(&idr); idr_alloc(&idr); /* 2,3,4 */
    int id99 = idr_alloc(&idr); /* 5 */

    (void)id5; (void)id10; (void)id99;

    TEST("idr_find: allocated ID returns 1", idr_find(&idr, 0));
    TEST("idr_find: allocated ID 1", idr_find(&idr, 1));
    TEST("idr_find: unallocated 6 returns 0", !idr_find(&idr, 6));

    /* Remove and re-check */
    idr_remove(&idr, 0);
    TEST("idr_find: after remove returns 0", !idr_find(&idr, 0));

    /* Re-allocate — should get ID 0 back (lowest free) */
    int new_id = idr_alloc(&idr);
    TEST("idr_alloc: after remove reuses ID", new_id == 0);

    /* Out of range remove — no crash */
    idr_remove(&idr, 200);
    TEST("idr_remove: out-of-range no crash", 1);

    /* Remove already free ID — no crash */
    idr_remove(&idr, 6);
    TEST("idr_remove: free ID no crash", 1);
}

/* ===================================================================
 *  test_idr_boundary
 * =================================================================== */
static void test_idr_boundary(void)
{
    /* Single-ID IDR */
    struct idr idr;
    idr_init(&idr, 1);
    TEST("idr_alloc: max=1 returns 0", idr_alloc(&idr) == 0);
    TEST("idr_alloc: max=1 full returns -1", idr_alloc(&idr) == -1);
    TEST("idr_find: id 0 allocated", idr_find(&idr, 0));
    idr_remove(&idr, 0);
    TEST("idr_alloc: max=1 after remove", idr_alloc(&idr) == 0);

    /* 64-bit boundary: exactly one word */
    struct idr idr2;
    idr_init(&idr2, 64);
    for (int i = 0; i < 64; i++) idr_alloc(&idr2);
    TEST("idr_alloc: 64 entries full", idr_alloc(&idr2) == -1);

    /* ID 63 check */
    TEST("idr_find: id 63 allocated", idr_find(&idr2, 63));
    idr_remove(&idr2, 63);
    TEST("idr_find: id 63 freed", !idr_find(&idr2, 63));
    TEST("idr_alloc: reuses id 63", idr_alloc(&idr2) == 63);

    /* max=2 (single word edge) */
    struct idr idr3;
    idr_init(&idr3, 2);
    TEST("idr_alloc: max=2 id 0", idr_alloc(&idr3) == 0);
    TEST("idr_alloc: max=2 id 1", idr_alloc(&idr3) == 1);
    TEST("idr_alloc: max=2 full", idr_alloc(&idr3) == -1);
    idr_remove(&idr3, 0);
    TEST("idr_alloc: max=2 reuses 0", idr_alloc(&idr3) == 0);

    /* max=63 (word boundary - 1) */
    struct idr idr4;
    idr_init(&idr4, 63);
    for (int i = 0; i < 63; i++) idr_alloc(&idr4);
    TEST("idr_alloc: max=63 full", idr_alloc(&idr4) == -1);
    idr_remove(&idr4, 62);
    TEST("idr_alloc: max=63 reuses 62", idr_alloc(&idr4) == 62);

    /* max=128 (two words) */
    struct idr idr5;
    idr_init(&idr5, 128);
    /* Allocate all 128 */
    for (int i = 0; i < 128; i++) idr_alloc(&idr5);
    TEST("idr_alloc: max=128 full", idr_alloc(&idr5) == -1);
    idr_remove(&idr5, 127);
    TEST("idr_alloc: max=128 reuses 127", idr_alloc(&idr5) == 127);

    /* alloc-remove-realloc cycle across word boundary */
    struct idr idr6;
    idr_init(&idr6, 65);
    for (int i = 0; i < 65; i++) idr_alloc(&idr6);
    TEST("idr_alloc: max=65 full", idr_alloc(&idr6) == -1);
    idr_remove(&idr6, 0);
    idr_remove(&idr6, 64);
    TEST("idr_alloc: max=65 reuses 0 after removal", idr_alloc(&idr6) == 0);
    TEST("idr_alloc: max=65 reuses 64", idr_alloc(&idr6) == 64);
}

/* ===================================================================
 *  test_idr_advanced — more edge cases
 * =================================================================== */
static void test_idr_advanced(void)
{
    struct idr idr;

    /* 1. idr_alloc with NULL idr */
    TEST("idr_advanced: alloc NULL returns -1", idr_alloc(NULL) == -1);

    /* 2. idr_find on empty IDR */
    idr_init(&idr, 64);
    TEST("idr_advanced: find on empty returns 0", !idr_find(&idr, 0));
    TEST("idr_advanced: find on empty id=63", !idr_find(&idr, 63));

    /* 3. idr_remove of non-existent IDs (no crash) */
    idr_remove(&idr, 50);
    TEST("idr_advanced: remove non-existent no crash", 1);
    idr_remove(&idr, -1);
    TEST("idr_advanced: remove -1 no crash", 1);
    idr_remove(&idr, 64);
    TEST("idr_advanced: remove >= max no crash", 1);

    /* 4. idr_find with out-of-range IDs */
    TEST("idr_advanced: find id >= max", !idr_find(&idr, 64));
    TEST("idr_advanced: find negative id", !idr_find(&idr, -5));

    /* 5. Fill entire IDR, remove some, verify re-use pattern */
    for (int i = 0; i < 64; i++) idr_alloc(&idr);
    TEST("idr_advanced: full returns -1", idr_alloc(&idr) == -1);
    idr_remove(&idr, 10);
    idr_remove(&idr, 20);
    idr_remove(&idr, 30);
    TEST("idr_advanced: removed 10 not found", !idr_find(&idr, 10));
    TEST("idr_advanced: removed 20 not found", !idr_find(&idr, 20));
    TEST("idr_advanced: removed 30 not found", !idr_find(&idr, 30));
    TEST("idr_advanced: still-allocated 0 found", idr_find(&idr, 0));

    /* Re-alloc should return lowest free: 10 */
    int new_id = idr_alloc(&idr);
    TEST("idr_advanced: reuses lowest free ID", new_id == 10);

    /* Remove last ID (63) — next alloc returns 20 (next lowest free) */
    idr_remove(&idr, 63);
    TEST("idr_advanced: reuses freed 20 next", idr_alloc(&idr) == 20);

    /* 6. Non-word-aligned max (200) */
    struct idr idr2;
    idr_init(&idr2, 200);
    int all_ok = 1;
    for (int i = 0; i < 200; i++) {
        if (idr_alloc(&idr2) != i) { all_ok = 0; break; }
    }
    TEST("idr_advanced: max=200 sequential", all_ok);
    TEST("idr_advanced: max=200 full", idr_alloc(&idr2) == -1);
    idr_remove(&idr2, 0);
    TEST("idr_advanced: max=200 reuses 0", idr_alloc(&idr2) == 0);

    idr_destroy(&idr);
    idr_destroy(&idr2);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== IDR (Integer ID Allocator) Tests ===\n\n");

    printf("--- idr_init ---\n");
    test_idr_init();

    printf("\n--- idr_alloc ---\n");
    test_idr_alloc();

    printf("\n--- idr_remove / idr_find ---\n");
    test_idr_remove_find();

    printf("\n--- Boundary ---\n");
    test_idr_boundary();

    printf("\n--- Advanced ---\n");
    test_idr_advanced();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
