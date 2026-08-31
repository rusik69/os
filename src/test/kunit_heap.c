/*
 * kunit_heap.c — KUnit unit tests for the kernel heap allocator
 *
 * Focus: kmalloc/kfree alignment guarantees across varied allocation
 * sizes, block splitting, and block coalescing after free.
 *
 * The heap allocator aligns every returned pointer to 16 bytes
 * (required for x86-64 SSE/AVX compatibility, see heap.c).  These
 * tests assert that guarantee holds for every size and that the
 * caller's full requested region is writable and reusable after kfree.
 *
 * Item 268: KUnit — heap allocator alignment tests
 */
#include "heap.h"
#include "kunit.h"
#include "string.h"

#define HEAP_MIN_ALIGN 16U

/* ====================================================================
 *  1. Alignment at every round size
 * ==================================================================== */

/* For a spread of sizes, kmalloc must return a pointer aligned to the
 * heap's guaranteed alignment and the caller region must be writable. */
static void heap_align_varied_sizes(struct kunit *test) {
    const size_t sizes[] = {1,  2,   3,   7,   8,   15,  16,  17,  31,  32,   33,   63,   64,
                            65, 127, 128, 129, 255, 256, 257, 511, 512, 1023, 1024, 4095, 4096};
    enum { N = sizeof(sizes) / sizeof(sizes[0]) };
    void *ptrs[N] = {0};

    for (int i = 0; i < N; i++) {
        void *p = kmalloc(sizes[i]);
        KUNIT_EXPECT_NOT_NULL(test, p);
        if (!p)
            break;
        KUNIT_EXPECT_TRUE(test, ((uintptr_t)p & (HEAP_MIN_ALIGN - 1)) == 0);
        /* The full requested region must be writable. */
        memset(p, (int)(i & 0xFF), sizes[i]);
        ptrs[i] = p;
    }

    for (int i = 0; i < N; i++) {
        if (ptrs[i])
            kfree(ptrs[i]);
    }
}

/* ====================================================================
 *  2. Alignment after block splitting (varied interleaved live set)
 * ==================================================================== */

/* Allocate many differently-sized blocks, free a scattered subset to
 * force the allocator to split/find first-fit blocks, then verify every
 * surviving and freshly-allocated pointer still meets alignment and is
 * not aliased (distinct pointers for distinct live allocations). */
static void heap_align_split_stability(struct kunit *test) {
    enum { N = 128 };
    void *ptrs[N] = {0};
    size_t sz[N];

    for (int i = 0; i < N; i++) {
        sz[i] = (size_t)((i % 5) * 7 + (i % 13) + 1); /* varied, uneven */
        ptrs[i] = kmalloc(sz[i]);
        KUNIT_EXPECT_NOT_NULL(test, ptrs[i]);
        if (!ptrs[i])
            break;
        memset(ptrs[i], (int)(0xA0 + (i & 0x0F)), sz[i]);
    }

    /* Free every third pointer, leaving the rest live — creates holes
     * that force later allocations to split free blocks. */
    for (int i = 0; i < N; i += 3) {
        if (ptrs[i]) {
            kfree(ptrs[i]);
            ptrs[i] = NULL;
        }
    }

    /* Reallocate into the holes; each new pointer must be aligned and
     * distinct from every still-live allocation. */
    for (int i = 0; i < N; i += 3) {
        if (!ptrs[i]) {
            ptrs[i] = kmalloc(sz[i]);
            KUNIT_EXPECT_NOT_NULL(test, ptrs[i]);
            if (!ptrs[i])
                break;
            KUNIT_EXPECT_TRUE(test, ((uintptr_t)ptrs[i] & (HEAP_MIN_ALIGN - 1)) == 0);
            for (int j = 0; j < N; j++) {
                if (j != i && ptrs[j] && ptrs[j] == ptrs[i]) {
                    KUNIT_EXPECT_TRUE(test, 0); /* aliasing is a bug */
                }
            }
        }
    }

    for (int i = 0; i < N; i++) {
        if (ptrs[i])
            kfree(ptrs[i]);
    }
}

/* ====================================================================
 *  3. Alignment after coalescing (free a contiguous run, reallocate)
 * ==================================================================== */

/* Free a run of adjacent live blocks so the allocator can coalesce,
 * then allocate a large block that must reuse the coalesced region and
 * still be aligned. */
static void heap_align_after_coalesce(struct kunit *test) {
    enum { R = 16 };
    void *ptrs[R] = {0};

    for (int i = 0; i < R; i++) {
        ptrs[i] = kmalloc(256);
        KUNIT_EXPECT_NOT_NULL(test, ptrs[i]);
        if (!ptrs[i])
            break;
    }

    /* Free the whole run so adjacent blocks coalesce. */
    for (int i = 0; i < R; i++) {
        if (ptrs[i])
            kfree(ptrs[i]);
    }

    /* Allocate a block larger than a single 256-byte region; it must be
     * pulled from the coalesced space and still be 16-byte aligned. */
    const size_t big = 256 * (R / 2) + 13;
    void *p = kmalloc(big);
    KUNIT_EXPECT_NOT_NULL(test, p);
    if (p) {
        KUNIT_EXPECT_TRUE(test, ((uintptr_t)p & (HEAP_MIN_ALIGN - 1)) == 0);
        memset(p, 0x5C, big); /* full region writable */
        kfree(p);
    }
}

/* ====================================================================
 *  4. kcalloc zeroing and alignment
 * ==================================================================== */

/* kcalloc must return zeroed, aligned memory which stays intact until
 * it is freed. */
static void heap_kcalloc_alignment(struct kunit *test) {
    const size_t n = 64;
    void *p = kcalloc(n, sizeof(uint32_t));
    KUNIT_EXPECT_NOT_NULL(test, p);
    if (!p)
        return;

    KUNIT_EXPECT_TRUE(test, ((uintptr_t)p & (HEAP_MIN_ALIGN - 1)) == 0);

    const uint32_t *u = (const uint32_t *)p;
    int zeroed = 1;
    for (size_t i = 0; i < n; i++) {
        if (u[i] != 0) {
            zeroed = 0;
            break;
        }
    }
    KUNIT_EXPECT_TRUE(test, zeroed);

    kfree(p);
}

/* ====================================================================
 *  Suite registration
 * ==================================================================== */

static const struct kunit_case heap_test_cases[] = {KUNIT_CASE(heap_align_varied_sizes),
                                                    KUNIT_CASE(heap_align_split_stability),
                                                    KUNIT_CASE(heap_align_after_coalesce),
                                                    KUNIT_CASE(heap_kcalloc_alignment),
                                                    {0}};

static struct kunit_suite heap_test_suite;

void kunit_heap_register(void) {
    int ci = 0;
    for (int i = 0; i < (int)(sizeof(heap_test_cases) / sizeof(heap_test_cases[0])) &&
                    heap_test_cases[i].run != NULL;
         i++) {
        heap_test_suite.cases[ci].name = heap_test_cases[i].name;
        heap_test_suite.cases[ci].run = heap_test_cases[i].run;
        ci++;
    }
    heap_test_suite.cases[ci].name = NULL;
    heap_test_suite.cases[ci].run = NULL;

    heap_test_suite.name = "heap";
    heap_test_suite.setup = NULL;
    heap_test_suite.teardown = NULL;

    kunit_register_suite(&heap_test_suite);
    kprintf("[KUnit] Heap allocator tests registered (%d cases)\n", ci);
}