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

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
