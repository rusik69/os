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
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Memory Pool Tests ===\n\n");

    printf("--- mempool_create/alloc/free/destroy ---\n");
    test_mempool();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
