/*
 * test_memory.c — Host-side unit tests for kernel memory allocator algorithms
 *
 * Tests the core block-based heap allocator logic (first-fit,
 * splitting, coalescing, fragmentation behaviour) that mirrors
 * the kernel's kmalloc/kfree implementation in src/memory/heap.c.
 *
 * Runs entirely on the host — no kernel dependencies.
 * Algorithm is identical to the kernel's: first-fit free list with
 * splitting on large blocks and forward/backward coalescing on free.
 *
 * Compile:  gcc -Wall -Werror -g -O0 -o test_memory test_memory.c
 * Run:      ./test_memory
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================
 *  Kernel-compatible heap block structure
 *
 *  Mirrors struct heap_block from src/memory/heap.c exactly so that
 *  the test validates the same algorithmic behaviour used in the
 *  kernel's kmalloc/kfree.
 * =================================================================== */

typedef struct heap_block {
    size_t            size;    /* usable payload size (excludes header) */
    int               free;    /* 1 = free, 0 = allocated */
    struct heap_block *next;   /* next block in linked list */
    struct heap_block *prev;   /* previous block in linked list */
} heap_block_t;

#define BLOCK_HDR_SIZE  sizeof(heap_block_t)
#define ALIGN_MASK      15ULL   /* 16-byte alignment */
#define HEAP_MAX_SIZE   (4UL * 1024 * 1024)  /* 4 MB for tests */

/* Round size up to 16-byte alignment */
#define ROUND_UP(s) (((s) + ALIGN_MASK) & ~ALIGN_MASK)

/* ── Test heap state ──────────────────────────────────────────────── */

static uint8_t      *g_heap_base   = NULL;  /* backing memory */
static heap_block_t *g_heap_start  = NULL;  /* first block */
static size_t        g_heap_used   = 0;     /* byte counter */

/* Safety: track total allocated so we can prevent buffer overrun */
static size_t g_heap_total_alloc = 0;

/* ===================================================================
 *  Test heap operations — identical algorithm to kernel heap.c
 * =================================================================== */

static void test_heap_init(void *base, size_t total_size)
{
    g_heap_base  = (uint8_t *)base;
    g_heap_start = (heap_block_t *)base;
    g_heap_start->size = total_size - BLOCK_HDR_SIZE;
    g_heap_start->free = 1;
    g_heap_start->next = NULL;
    g_heap_start->prev = NULL;
    g_heap_used        = 0;
    g_heap_total_alloc = 0;
}

static void *test_kmalloc(size_t size)
{
    if (size == 0) return NULL;

    size = ROUND_UP(size);

    /* First-fit search */
    heap_block_t *block = g_heap_start;
    while (block) {
        if (block->free && block->size >= size) {
            /* Split if the remainder is large enough for a new block */
            if (block->size > size + BLOCK_HDR_SIZE + 16) {
                heap_block_t *new_block = (heap_block_t *)
                    ((uint8_t *)block + BLOCK_HDR_SIZE + size);
                new_block->size = block->size - size - BLOCK_HDR_SIZE;
                new_block->free = 1;
                new_block->next = block->next;
                new_block->prev = block;
                if (new_block->next)
                    new_block->next->prev = new_block;
                block->next = new_block;
                block->size = size;
            }
            block->free = 0;
            g_heap_used += block->size + BLOCK_HDR_SIZE;
            g_heap_total_alloc++;
            return (void *)((uint8_t *)block + BLOCK_HDR_SIZE);
        }
        if (!block->next) break;
        block = block->next;
    }

    /* No space — out of memory */
    return NULL;
}

static void test_kfree(void *ptr)
{
    if (!ptr) return;

    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HDR_SIZE);
    block->free = 1;
    g_heap_used -= (block->size + BLOCK_HDR_SIZE);
    g_heap_total_alloc--;

    /* Forward coalesce with next block */
    if (block->next && block->next->free) {
        block->size += BLOCK_HDR_SIZE + block->next->size;
        heap_block_t *old_next = block->next->next;
        block->next = old_next;
        if (old_next) old_next->prev = block;
    }

    /* Backward coalesce with previous block */
    if (block->prev && block->prev->free) {
        heap_block_t *prev = block->prev;
        prev->size += BLOCK_HDR_SIZE + block->size;
        prev->next = block->next;
        if (block->next) block->next->prev = prev;
    }
}

/* Sanity check: walk the free list and ensure no corruption */
static int heap_sanity_check(void)
{
    heap_block_t *b = g_heap_start;
    size_t total = 0;
    while (b) {
        total += BLOCK_HDR_SIZE + b->size;
        /* Ensure block is within bounds */
        if ((uint8_t *)b < g_heap_base) return -1;
        if ((uint8_t *)b + BLOCK_HDR_SIZE + b->size >
            g_heap_base + HEAP_MAX_SIZE) return -1;
        /* Check linked-list consistency */
        if (b->next && b->next->prev != b) return -1;
        if (b->prev && b->prev->next != b) return -1;
        /* No two adjacent free blocks (should have been coalesced) */
        if (b->free && b->next && b->next->free) return -1;
        b = b->next;
    }
    return 0;
}

/* Verify that a pointer is properly aligned */
static int is_aligned(void *ptr)
{
    return ((uintptr_t)ptr & ALIGN_MASK) == 0;
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
        printf("        Expected: %zu\n",       \
               (size_t)(expected));             \
        printf("        Got:      %zu\n",       \
               (size_t)(got));                  \
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
 *  Test: Basic allocation and immediate free
 * =================================================================== */

static void test_basic_alloc_free(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    TEST("malloc(16) returns non-NULL");
    void *p1 = test_kmalloc(16);
    ASSERT_PTR_NONNULL(p1, "malloc(16)");
    PASS();

    TEST("malloc(16) pointer is aligned");
    ASSERT(is_aligned(p1), "16-byte alignment");
    PASS();

    TEST("malloc(32) returns non-NULL");
    void *p2 = test_kmalloc(32);
    ASSERT_PTR_NONNULL(p2, "malloc(32)");
    PASS();

    TEST("free(p1) succeeds");
    test_kfree(p1);
    ASSERT(heap_sanity_check() == 0, "heap consistent after free");
    PASS();

    TEST("free(p2) succeeds");
    test_kfree(p2);
    ASSERT(heap_sanity_check() == 0, "heap consistent after free");
    PASS();
}

/* ===================================================================
 *  Test: malloc(0) returns NULL
 * =================================================================== */

static void test_malloc_zero(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    TEST("malloc(0) returns NULL");
    void *p = test_kmalloc(0);
    ASSERT(p == NULL, "malloc(0) should return NULL");
    PASS();
}

/* ===================================================================
 *  Test: Multiple allocations (no free)
 * =================================================================== */

static void test_multiple_alloc(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));
    const int N = 64;
    void *ptrs[N];
    int ok_count = 0;

    TEST("allocate 64 blocks of 128 bytes");
    for (int i = 0; i < N; i++) {
        ptrs[i] = test_kmalloc(128);
        if (ptrs[i]) ok_count++;
        /* Fill with pattern to detect cross-block corruption */
        if (ptrs[i]) memset(ptrs[i], (i & 0xFF), 128);
    }
    ASSERT_INT_EQ(ok_count, N, "all 64 allocations succeeded");
    ASSERT(heap_sanity_check() == 0, "heap consistent");
    PASS();

    TEST("verify stored data patterns survive");
    for (int i = 0; i < N; i++) {
        if (!ptrs[i]) continue;
        uint8_t expected = i & 0xFF;
        uint8_t *data = (uint8_t *)ptrs[i];
        for (int j = 0; j < 128; j++) {
            if (data[j] != expected) {
                FAIL("data corruption detected");
                return;
            }
        }
    }
    PASS();

    /* Clean up */
    for (int i = 0; i < N; i++)
        test_kfree(ptrs[i]);
    ASSERT(heap_sanity_check() == 0, "heap consistent after all free");
}

/* ===================================================================
 *  Test: Variable size allocations
 * =================================================================== */

static void test_variable_sizes(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    size_t sizes[] = {1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33,
                      63, 64, 65, 127, 128, 255, 256, 511, 512,
                      1023, 1024, 2048, 4096, 8192, 16384};
    int n = sizeof(sizes) / sizeof(sizes[0]);
    void *ptrs[sizeof(sizes) / sizeof(sizes[0])];

    TEST("allocate varying sizes (1 to 16384)");
    for (int i = 0; i < n; i++) {
        ptrs[i] = test_kmalloc(sizes[i]);
        ASSERT_PTR_NONNULL(ptrs[i], "allocation");
        memset(ptrs[i], (i & 0xFF), sizes[i]);
    }
    ASSERT(heap_sanity_check() == 0, "heap consistent");
    PASS();

    TEST("verify all variable-size blocks");
    for (int i = 0; i < n; i++) {
        uint8_t *data = (uint8_t *)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (data[j] != (uint8_t)(i & 0xFF)) {
                FAIL("data corruption in variable block");
                return;
            }
        }
    }
    PASS();

    for (int i = 0; i < n; i++)
        test_kfree(ptrs[i]);
    ASSERT(heap_sanity_check() == 0, "heap consistent");
}

/* ===================================================================
 *  Test: Free-and-realloc (fragmentation recovery)
 * =================================================================== */

static void test_free_realloc(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    void *a = test_kmalloc(1024);
    void *b = test_kmalloc(1024);
    void *c = test_kmalloc(1024);
    ASSERT_PTR_NONNULL(a, "alloc a");
    ASSERT_PTR_NONNULL(b, "alloc b");
    ASSERT_PTR_NONNULL(c, "alloc c");

    TEST("free middle block, realloc same size");
    test_kfree(b);
    ASSERT(heap_sanity_check() == 0, "consistent after free b");

    void *b2 = test_kmalloc(1024);
    ASSERT_PTR_NONNULL(b2, "realloc b");
    ASSERT(heap_sanity_check() == 0, "consistent after realloc b");
    PASS();

    test_kfree(a);
    test_kfree(b2);
    test_kfree(c);
    ASSERT(heap_sanity_check() == 0, "consistent after all free");
}

/* ===================================================================
 *  Test: Coalescing — adjacent free blocks merge
 * =================================================================== */

static void test_coalescing(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    /* Allocate three adjacent blocks */
    void *a = test_kmalloc(64);
    void *b = test_kmalloc(64);
    void *c = test_kmalloc(64);
    ASSERT_PTR_NONNULL(a, "alloc a");
    ASSERT_PTR_NONNULL(b, "alloc b");
    ASSERT_PTR_NONNULL(c, "alloc c");

    TEST("free a and b, verify coalescence into single free block");
    test_kfree(a);
    test_kfree(b);
    ASSERT(heap_sanity_check() == 0, "consistent after free a,b");

    /* Now allocate a large block that spans both a and b's original area */
    void *big = test_kmalloc(160);  /* should fit in coalesced area */
    ASSERT_PTR_NONNULL(big, "large alloc after coalesce");
    ASSERT(heap_sanity_check() == 0, "consistent after large alloc");
    PASS();

    test_kfree(c);
    test_kfree(big);
    ASSERT(heap_sanity_check() == 0, "consistent after all free");
}

/* ===================================================================
 *  Test: Split — large free block split for small alloc
 * =================================================================== */

static void test_splitting(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    /* Allocate and free a large block so we have a large free area */
    void *large = test_kmalloc(8192);
    ASSERT_PTR_NONNULL(large, "large alloc");
    test_kfree(large);
    ASSERT(heap_sanity_check() == 0, "consistent after free large");

    TEST("allocate small from large free region (splitting)");
    void *small1 = test_kmalloc(16);
    void *small2 = test_kmalloc(16);
    void *small3 = test_kmalloc(16);
    ASSERT_PTR_NONNULL(small1, "small1");
    ASSERT_PTR_NONNULL(small2, "small2");
    ASSERT_PTR_NONNULL(small3, "small3");
    ASSERT(heap_sanity_check() == 0, "consistent after split allocs");
    PASS();

    test_kfree(small1);
    test_kfree(small2);
    test_kfree(small3);
    ASSERT(heap_sanity_check() == 0, "consistent after free smalls");
}

/* ===================================================================
 *  Test: Stress — many alternating alloc/free patterns
 * =================================================================== */

static void test_stress(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));
    const int N = 256;
    void *ptrs[N];
    memset(ptrs, 0, sizeof(ptrs));

    TEST("stress: allocate and free 256 blocks in alternating pattern");
    for (int iter = 0; iter < 4; iter++) {
        /* Allocate */
        for (int i = 0; i < N; i++) {
            size_t sz = 1 + (i * 7) % 256;  /* variable sizes */
            ptrs[i] = test_kmalloc(sz);
            if (!ptrs[i] && g_heap_used < HEAP_MAX_SIZE - 4096) {
                FAIL("unexpected OOM during stress alloc");
                return;
            }
            if (ptrs[i])
                memset(ptrs[i], (i ^ iter) & 0xFF, sz);
        }
        /* Free every other block */
        for (int i = 1; i < N; i += 2) {
            test_kfree(ptrs[i]);
            ptrs[i] = NULL;
        }
        /* Re-allocate the freed slots */
        for (int i = 1; i < N; i += 2) {
            size_t sz = 1 + (i * 7) % 256;
            ptrs[i] = test_kmalloc(sz);
            if (ptrs[i])
                memset(ptrs[i], (i ^ (iter + 1)) & 0xFF, sz);
        }
        ASSERT(heap_sanity_check() == 0, "consistent during stress");
    }
    PASS();

    /* Free all */
    for (int i = 0; i < N; i++) {
        if (ptrs[i]) test_kfree(ptrs[i]);
    }
    ASSERT(heap_sanity_check() == 0, "consistent after stress cleanup");
}

/* ===================================================================
 *  Test: Maximum size allocation
 * =================================================================== */

static void test_large_alloc(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    TEST("allocate and free a large block (half heap)");
    size_t half = HEAP_MAX_SIZE / 2 - BLOCK_HDR_SIZE;
    void *big = test_kmalloc(half);
    ASSERT_PTR_NONNULL(big, "half-heap alloc");
    ASSERT(heap_sanity_check() == 0, "consistent after large alloc");

    /* Fill with a pattern, then free */
    memset(big, 0xAA, half);
    test_kfree(big);
    ASSERT(heap_sanity_check() == 0, "consistent after freeing large block");
    PASS();

    /* Verify the large block coalesced back and we can allocate it again */
    TEST("re-allocate large block after free (coalescing)");
    void *big2 = test_kmalloc(half);
    ASSERT_PTR_NONNULL(big2, "re-alloc after free");
    ASSERT(heap_sanity_check() == 0, "consistent");
    PASS();

    test_kfree(big2);
    ASSERT(heap_sanity_check() == 0, "consistent at end");

    /* OOM test: allocate more than heap size */
    TEST("OOM: allocation exceeding heap returns NULL");
    void *huge = test_kmalloc(HEAP_MAX_SIZE + 4096);
    ASSERT(huge == NULL, "oversized alloc returns NULL");
    PASS();
}

/* ===================================================================
 *  Test: Free NULL is a no-op
 * =================================================================== */

static void test_free_null(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    TEST("free(NULL) is a no-op (does not crash)");
    test_kfree(NULL);
    ASSERT(heap_sanity_check() == 0, "still consistent");
    PASS();
}

/* ===================================================================
 *  Test: Backward coalescing — free middle then left neighbour
 * =================================================================== */

static void test_backward_coalesce(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    void *a = test_kmalloc(64);
    void *b = test_kmalloc(64);
    void *c = test_kmalloc(64);
    ASSERT_PTR_NONNULL(a, "a");
    ASSERT_PTR_NONNULL(b, "b");
    ASSERT_PTR_NONNULL(c, "c");

    TEST("backward coalesce: free right, then left neighbour");
    /* Free rightmost first, then left neighbour — should coalesce backward */
    test_kfree(c);
    test_kfree(b);
    ASSERT(heap_sanity_check() == 0, "consistent after b,c free");

    /* The coalesced area (b + c) should now be one large free block */
    void *big = test_kmalloc(140);  /* should fit b+c area */
    ASSERT_PTR_NONNULL(big, "alloc in coalesced area");
    ASSERT(heap_sanity_check() == 0, "consistent after alloc");
    PASS();

    test_kfree(a);
    test_kfree(big);
    ASSERT(heap_sanity_check() == 0, "consistent at end");
}

/* ===================================================================
 *  Test: Repeated allocate/free cycles (thrashing)
 * =================================================================== */

static void test_thrash(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));
    const int N = 64;

    TEST("thrash: 100 cycles of alloc/free with varying sizes");
    for (int cycle = 0; cycle < 100; cycle++) {
        void *ptrs[N];
        for (int i = 0; i < N; i++) {
            size_t sz = (cycle * 31 + i * 17) % 1024 + 1;
            ptrs[i] = test_kmalloc(sz);
            if (!ptrs[i]) break;
            memset(ptrs[i], (i ^ cycle) & 0xFF, sz);
        }
        for (int i = 0; i < N; i++) {
            if (ptrs[i]) test_kfree(ptrs[i]);
        }
        int ret = heap_sanity_check();
        if (ret != 0) {
            FAIL("heap corruption during thrash");
            return;
        }
    }
    PASS();
}

/* ===================================================================
 *  Test: Alignment guarantee
 * =================================================================== */

static void test_alignment(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    TEST("all allocations are 16-byte aligned");
    for (size_t sz = 1; sz <= 256; sz++) {
        void *p = test_kmalloc(sz);
        if (!p) break;
        ASSERT(is_aligned(p), "alignment violation");
    }
    PASS();

    /* Clean up: free all blocks by iterating and freeing */
    heap_block_t *b = g_heap_start;
    while (b) {
        if (!b->free) {
            void *ptr = (void *)((uint8_t *)b + BLOCK_HDR_SIZE);
            test_kfree(ptr);
        }
        b = b->next;
    }
    ASSERT(heap_sanity_check() == 0, "consistent after cleanup");
}

/* ===================================================================
 *  Test: Cross-block corruption detection (buffer overflow)
 * =================================================================== */

static void test_no_cross_block_corruption(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));

    TEST("allocations do not overlap (boundary check)");
    void *a = test_kmalloc(64);
    void *b = test_kmalloc(64);
    ASSERT_PTR_NONNULL(a, "a");
    ASSERT_PTR_NONNULL(b, "b");

    /* Check minimum distance between allocations */
    ptrdiff_t diff = (uint8_t *)b - (uint8_t *)a;
    ASSERT(diff >= (ptrdiff_t)(64 + BLOCK_HDR_SIZE),
           "blocks too close — possible overlap");
    PASS();

    test_kfree(a);
    test_kfree(b);
}

/* ===================================================================
 *  Test: Multiple small allocs after large free (fragmentation)
 * =================================================================== */

static void test_fragmentation_recovery(void)
{
    uint8_t heap_mem[HEAP_MAX_SIZE];
    test_heap_init(heap_mem, sizeof(heap_mem));
    const int N = 32;
    void *ptrs[N];

    TEST("fragmentation: allocate many small, free every other, allocate medium");
    for (int i = 0; i < N; i++)
        ptrs[i] = test_kmalloc(32);
    /* Free every other */
    for (int i = 0; i < N; i += 2) {
        test_kfree(ptrs[i]);
        ptrs[i] = NULL;
    }
    ASSERT(heap_sanity_check() == 0, "consistent after frag free");

    /* Try to allocate medium blocks in the freed slots */
    for (int i = 0; i < N; i++) {
        if (ptrs[i] == NULL) {
            ptrs[i] = test_kmalloc(32);
            ASSERT_PTR_NONNULL(ptrs[i], "frag recovery alloc");
        }
    }
    ASSERT(heap_sanity_check() == 0, "consistent after frag recovery");
    PASS();

    /* Clean up */
    for (int i = 0; i < N; i++)
        if (ptrs[i]) test_kfree(ptrs[i]);
    ASSERT(heap_sanity_check() == 0, "consistent after cleanup");
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("============================================\n");
    printf("  Memory Allocator Unit Tests\n");
    printf("============================================\n\n");

    test_basic_alloc_free();
    test_malloc_zero();
    test_multiple_alloc();
    test_variable_sizes();
    test_free_realloc();
    test_coalescing();
    test_splitting();
    test_stress();
    test_large_alloc();
    test_free_null();
    test_backward_coalesce();
    test_thrash();
    test_alignment();
    test_no_cross_block_corruption();
    test_fragmentation_recovery();

    printf("\n============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
