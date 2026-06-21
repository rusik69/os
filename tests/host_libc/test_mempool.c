/*
 * test_mempool.c — Host-side tests for kernel memory pool
 *
 * Tests mempool_create, mempool_alloc, mempool_free, mempool_destroy
 * from src/lib/mempool.c. Uses kmalloc/kfree from stubs.o.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel type declarations (mirror kernel mempool.h)
 * =================================================================== */
typedef struct { void **elements; int cur_nr, max_nr, elem_size, min_nr; } mempool_t;

extern mempool_t *mempool_create(int min_nr, int elem_size);
extern void *mempool_alloc(mempool_t *pool);
extern void mempool_free(void *element, mempool_t *pool);
extern void mempool_destroy(mempool_t *pool);

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
 *  test_mempool
 * =================================================================== */
static void test_mempool(void)
{
    /* 1. Create with valid params */
    mempool_t *pool = mempool_create(10, 64);
    TEST("mempool_create(10,64) non-NULL", pool != NULL);
    if (!pool) return;

    /* 2. Alloc returns non-NULL */
    void *p1 = mempool_alloc(pool);
    TEST("mempool_alloc: non-NULL", p1 != NULL);

    /* 3. Alloc returns distinct pointers */
    void *p2 = mempool_alloc(pool);
    TEST("mempool_alloc: second non-NULL", p2 != NULL);
    TEST("mempool_alloc: distinct pointers", p1 != p2);

    /* 4. Free then alloc reuses the freed element */
    mempool_free(p1, pool);
    void *p1_re = mempool_alloc(pool);
    TEST("mempool_free+alloc: reuses element", p1_re == p1);

    /* 5. Free when pool is full calls kfree (alloc more than min_nr) */
    /* pool max_nr = 10*2 = 20, cur_nr after allocs: start 10, alloc two = 8,
     * free two = 10, then alloc from pool */
    /* Drain to empty */
    for (int i = 0; i < 20; i++) {
        void *v = mempool_alloc(pool);
        if (!v) { TEST("mempool_alloc: drain failed", 0); break; }
    }
    /* Alloc when empty — allocates via kmalloc */
    void *fresh = mempool_alloc(pool);
    TEST("mempool_alloc: from empty uses kmalloc", fresh != NULL);
    /* Free when pool full — calls kfree (destructively) */
    mempool_free(fresh, pool);
    /* Free another — should be stored (pool was at 0, now at 1) */
    void *extra = malloc(64);
    mempool_free(extra, pool);
    /* Alloc should return extra (most recently freed) */
    void *reclaimed = mempool_alloc(pool);
    TEST("mempool_free: reclaims most recently freed", reclaimed == extra);

    /* 6. Create with min_nr=0 */
    mempool_t *pool2 = mempool_create(0, 64);
    TEST("mempool_create(0,64) non-NULL", pool2 != NULL);
    if (pool2) {
        void *z = mempool_alloc(pool2);
        TEST("mempool_alloc: pool(0,64) works", z != NULL);
        mempool_free(z, pool2);
        mempool_destroy(pool2);
    }

    /* 7. Create with elem_size=0 */
    mempool_t *pool3 = mempool_create(5, 0);
    TEST("mempool_create(5,0) non-NULL", pool3 != NULL);
    if (pool3) {
        void *z = mempool_alloc(pool3);
        TEST("mempool_alloc: pool(5,0) works", z != NULL);
        mempool_free(z, pool3);
        mempool_destroy(pool3);
    }

    /* 8. Destroy does not crash */
    mempool_destroy(pool);

    /* 9. Alloc multiple from same pool */
    mempool_t *pool4 = mempool_create(4, 16);
    TEST("mempool_create(4,16) non-NULL", pool4 != NULL);
    if (pool4) {
        void *a = mempool_alloc(pool4);
        void *b = mempool_alloc(pool4);
        void *c = mempool_alloc(pool4);
        void *d = mempool_alloc(pool4);
        TEST("mempool_alloc: 4 allocs all non-NULL",
             a != NULL && b != NULL && c != NULL && d != NULL);
        TEST("mempool_alloc: 4 allocs distinct",
             a != b && a != c && a != d && b != c && b != d && c != d);
        mempool_free(a, pool4);
        mempool_free(b, pool4);
        mempool_free(c, pool4);
        mempool_free(d, pool4);
        mempool_destroy(pool4);
    }
}

/* ===================================================================
 *  test_mempool_more — additional edge cases
 * =================================================================== */
static void test_mempool_more(void)
{
    /* 1. Multiple allocs and frees in sequence (ping-pong) */
    {
        mempool_t *pool = mempool_create(5, 32);
        TEST("mempool_more: create(5,32) ok", pool != NULL);
        if (!pool) return;
        void *ptrs[10];
        int ok = 1;
        for (int i = 0; i < 10; i++) {
            ptrs[i] = mempool_alloc(pool);
            if (!ptrs[i]) { ok = 0; break; }
        }
        TEST("mempool_more: 10 allocs all non-NULL", ok);
        for (int i = 0; i < 10; i++) {
            if (ptrs[i]) mempool_free(ptrs[i], pool);
        }
        /* Re-alloc after freeing all — should reuse some */
        void *r1 = mempool_alloc(pool);
        TEST("mempool_more: re-alloc after free all", r1 != NULL);
        mempool_free(r1, pool);
        mempool_destroy(pool);
    }

    /* 2. Pool with very large min_nr (1000) */
    {
        mempool_t *pool = mempool_create(1000, 8);
        TEST("mempool_more: create(1000,8) ok", pool != NULL);
        if (!pool) return;
        /* Alloc more than pre-allocated */
        void *p = mempool_alloc(pool);
        TEST("mempool_more: alloc from large pool", p != NULL);
        mempool_free(p, pool);
        mempool_destroy(pool);
    }

    /* 3. Pool with very small elem_size (1 byte) */
    {
        mempool_t *pool = mempool_create(10, 1);
        TEST("mempool_more: create(10,1) ok", pool != NULL);
        if (!pool) return;
        void *p = mempool_alloc(pool);
        TEST("mempool_more: alloc 1-byte elem", p != NULL);
        /* Write to it to ensure it's usable */
        if (p) *(char*)p = 'x';
        mempool_free(p, pool);
        mempool_destroy(pool);
    }

    /* 4. Pool with large elem_size (4096) */
    {
        mempool_t *pool = mempool_create(4, 4096);
        TEST("mempool_more: create(4,4096) ok", pool != NULL);
        if (!pool) return;
        void *p = mempool_alloc(pool);
        TEST("mempool_more: alloc 4096-byte elem", p != NULL);
        if (p) memset(p, 0, 4096);
        mempool_free(p, pool);
        mempool_destroy(pool);
    }

    /* 5. (skipped — alloc from destroyed pool causes use-after-free,
       would need signal handling to test safely) */

    /* 6. Pool with min_nr=0, then alloc many elements */
    {
        mempool_t *pool = mempool_create(0, 32);
        TEST("mempool_more: create(0,32) ok", pool != NULL);
        if (!pool) return;
        void *a = mempool_alloc(pool);
        void *b = mempool_alloc(pool);
        void *c = mempool_alloc(pool);
        TEST("mempool_more: 3 allocs from min_nr=0 pool",
             a != NULL && b != NULL && c != NULL);
        mempool_free(a, pool);
        mempool_free(b, pool);
        mempool_free(c, pool);
        mempool_destroy(pool);
    }

    /* 7. Alloc exactly min_nr elements, free them, alloc again */
    {
        mempool_t *pool = mempool_create(8, 16);
        TEST("mempool_more: create(8,16) ok", pool != NULL);
        if (!pool) return;
        void *ptrs[8];
        int ok = 1;
        for (int i = 0; i < 8; i++) {
            ptrs[i] = mempool_alloc(pool);
            if (!ptrs[i]) ok = 0;
        }
        TEST("mempool_more: alloc all 8 pre-allocated", ok);
        for (int i = 0; i < 8; i++) {
            if (ptrs[i]) mempool_free(ptrs[i], pool);
        }
        /* Now re-alloc — should reuse pre-allocated elements */
        ok = 1;
        for (int i = 0; i < 8; i++) {
            ptrs[i] = mempool_alloc(pool);
            if (!ptrs[i]) ok = 0;
        }
        TEST("mempool_more: re-alloc all 8 after free", ok);
        for (int i = 0; i < 8; i++) {
            if (ptrs[i]) mempool_free(ptrs[i], pool);
        }
        mempool_destroy(pool);
    }

    /* 8. Alloc more than max_nr (force kmalloc path) */
    {
        mempool_t *pool = mempool_create(3, 16);
        TEST("mempool_more: create(3,16) for overflow", pool != NULL);
        if (!pool) return;
        /* Pool max_nr = 3*2=6, so alloc 10 */
        int ok = 1;
        for (int i = 0; i < 10; i++) {
            void *p = mempool_alloc(pool);
            if (!p) { ok = 0; break; }
            /* Intentionally NOT freeing — testing kmalloc fallback */
        }
        TEST("mempool_more: alloc beyond max_nr ok", ok);
        mempool_destroy(pool);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Memory Pool Tests ===\n\n");

    printf("--- mempool_create/alloc/free/destroy ---\n");
    test_mempool();

    printf("\n--- more edge cases ---\n");
    test_mempool_more();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
