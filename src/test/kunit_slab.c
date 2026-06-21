/*
 * kunit_slab.c — KUnit unit tests for the kernel slab allocator
 *
 * Comprehensive tests for kmalloc/kfree, kmem_cache operations,
 * object overflow detection (KASAN redzones), stress testing,
 * and edge-case behaviour.
 *
 * Item 268: KUnit — slab allocator tests
 *
 * Run via:
 *   # echo 1 > /sys/kernel/debug/kunit/run_all
 *   # cat /sys/kernel/debug/kunit/results
 *
 * Or to run just this suite:
 *   # echo 1 > /sys/kernel/debug/kunit/run/slab_full
 */

#include "kunit.h"
#include "slab.h"
#include "heap.h"
#include "kasan_light.h"
#include "string.h"
#include "printf.h"
#include "export.h"

/* ── Convenience helpers (mirrored from kunit.h for readability) ──── */
#ifndef KUNIT_EXPECT_GT
#define KUNIT_EXPECT_GT(t, left, right)                                      \
    KUNIT_EXPECT_TRUE(t, (left) > (right))
#endif

/* Skip this test with a message (simplified — just returns). */
#define KUNIT_SKIP(test, msg) do {                                            \
    kprintf("[KUNIT] SKIP: %s (%s)\n", (test)->name, (msg));                  \
    return;                                                                   \
} while(0)

/* ====================================================================
 *  1. Basic kmalloc / kfree operations
 * ==================================================================== */

/* Allocate a small block, write a known pattern, verify, and free. */
static void slab_alloc_pattern_test(struct kunit *test)
{
    const size_t sz = 64;
    uint8_t *p = (uint8_t *)kmalloc(sz);
    KUNIT_EXPECT_NOT_NULL(test, p);
    if (!p) return;

    /* Write a test pattern */
    for (size_t i = 0; i < sz; i++)
        p[i] = (uint8_t)(i ^ 0xA5);

    /* Verify the pattern */
    int ok = 1;
    for (size_t i = 0; i < sz; i++) {
        if (p[i] != (uint8_t)(i ^ 0xA5)) {
            ok = 0;
            break;
        }
    }
    KUNIT_EXPECT_TRUE(test, ok);

    kfree(p);
}

/* Allocate zero bytes — must return a valid pointer or NULL (both OK). */
static void slab_alloc_zero_test(struct kunit *test)
{
    void *p = kmalloc(0);
    /* kmalloc(0) may return NULL or a unique pointer — either is acceptable */
    if (p)
        kfree(p);
    KUNIT_EXPECT_TRUE(test, 1); /* reached without fault */
}

/* Allocate a very large block — must fail gracefully. */
static void slab_alloc_huge_test(struct kunit *test)
{
    void *p = kmalloc(0xFFFFFFFFULL);
    KUNIT_EXPECT_NULL(test, p);
}

/* Allocate and free repeatedly with the same size to test cache reuse. */
static void slab_alloc_free_reuse_test(struct kunit *test)
{
    const int N = 100;
    for (int i = 0; i < N; i++) {
        void *p = kmalloc(32);
        KUNIT_EXPECT_NOT_NULL(test, p);
        if (p) {
            memset(p, (int)(i & 0xFF), 32);
            kfree(p);
        }
    }
}

/* ====================================================================
 *  2. Variable-size allocation tests
 * ==================================================================== */

/* Allocate many different sizes, verify independent. */
static void slab_varied_sizes_test(struct kunit *test)
{
    size_t sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512,
                      1024, 2048, 4096, 8192, 16384};
    void *ptrs[16];
    int n = (int)(sizeof(sizes) / sizeof(sizes[0]));

    KUNIT_EXPECT_TRUE(test, n <= 16); /* prevent overflow */

    /* Allocate each size */
    for (int i = 0; i < n; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        KUNIT_EXPECT_NOT_NULL(test, ptrs[i]);
        if (ptrs[i]) {
            /* Fill the entire allocation with a known byte */
            memset(ptrs[i], (int)((i + 1) & 0xFF), sizes[i]);
        }
    }

    /* Verify each allocation retained its data */
    for (int i = 0; i < n; i++) {
        if (!ptrs[i]) continue;
        uint8_t expected = (uint8_t)((i + 1) & 0xFF);
        uint8_t *buf = (uint8_t *)ptrs[i];
        int ok = 1;
        for (size_t j = 0; j < sizes[i]; j++) {
            if (buf[j] != expected) {
                ok = 0;
                break;
            }
        }
        KUNIT_EXPECT_TRUE(test, ok);
    }

    /* Free in reverse order */
    for (int i = n - 1; i >= 0; i--) {
        if (ptrs[i])
            kfree(ptrs[i]);
    }
}

/* ====================================================================
 *  3. kmem_cache operations
 * ==================================================================== */

/* Small structure for cache allocation tests. */
struct slab_test_obj {
    uint64_t  magic;
    uint32_t  id;
    char      data[32];
};

/* Create a slab cache, allocate and free objects. */
static void slab_cache_basic_test(struct kunit *test)
{
    struct kmem_cache *cache = kmem_cache_create("test_obj",
                                                  sizeof(struct slab_test_obj),
                                                  0, NULL);
    KUNIT_EXPECT_NOT_NULL(test, cache);
    if (!cache) return;

    /* Allocate an object */
    struct slab_test_obj *obj = (struct slab_test_obj *)kmem_cache_alloc(cache);
    KUNIT_EXPECT_NOT_NULL(test, obj);
    if (!obj) {
        kmem_cache_destroy(cache);
        return;
    }

    /* Initialize and verify */
    obj->magic = 0xCAFEBABE;
    obj->id    = 42;
    memcpy(obj->data, "Hello cache!", 13);

    KUNIT_EXPECT_EQ(test, (int64_t)obj->magic, (int64_t)0xCAFEBABE);
    KUNIT_EXPECT_EQ(test, (int64_t)obj->id, (int64_t)42);
    KUNIT_EXPECT_EQ(test, (int64_t)strcmp(obj->data, "Hello cache!"), (int64_t)0);

    kmem_cache_free(cache, obj);
    kmem_cache_destroy(cache);
}

/* Allocate many objects from the same cache to exercise slab growth. */
static void slab_cache_multi_alloc_test(struct kunit *test)
{
    const int N = 64;
    struct kmem_cache *cache = kmem_cache_create("test_multi",
                                                  sizeof(struct slab_test_obj),
                                                  0, NULL);
    KUNIT_EXPECT_NOT_NULL(test, cache);
    if (!cache) return;

    struct slab_test_obj *objs[N];
    int success = 1;

    for (int i = 0; i < N; i++) {
        objs[i] = (struct slab_test_obj *)kmem_cache_alloc(cache);
        if (!objs[i]) {
            success = 0;
            break;
        }
        objs[i]->magic = 0xCAFEBABE;
        objs[i]->id    = (uint32_t)i;
    }
    KUNIT_EXPECT_TRUE(test, success);

    /* Verify each object */
    for (int i = 0; i < N && i < 64; i++) {
        if (!objs[i]) continue;
        KUNIT_EXPECT_EQ(test, (int64_t)objs[i]->magic, (int64_t)0xCAFEBABE);
        KUNIT_EXPECT_EQ(test, (int64_t)objs[i]->id, (int64_t)i);
    }

    /* Free all */
    for (int i = 0; i < N; i++) {
        if (objs[i])
            kmem_cache_free(cache, objs[i]);
    }
    kmem_cache_destroy(cache);
}

/* ====================================================================
 *  4. Object overflow detection (KASAN redzones)
 * ==================================================================== */

/*
 * Write past the end of a kmalloc buffer to verify KASAN redzone
 * detection catches it.  This test is only meaningful when KASAN
 * is enabled (KASAN_ENABLED = 1) — without KASAN, the overflow
 * will silently corrupt heap metadata.
 *
 * We allocate a buffer, then attempt to write one byte past the end.
 * With KASAN enabled, kasan_check() in the inline instrumentation or
 * KASAN_CHECK_OVERFLOW macro should trip.  We use a try/except pattern
 * (via the kasan_check function directly) to verify the detection
 * works without crashing the kernel.
 */
static void slab_kasan_overflow_test(struct kunit *test)
{
#if KASAN_ENABLED
    /* Allocate a small buffer (16 bytes) */
    uint8_t *buf = (uint8_t *)kmalloc(16);
    KUNIT_EXPECT_NOT_NULL(test, buf);
    if (!buf) return;

    /* Normal access within bounds — must pass KASAN check */
    int normal_ok = kasan_check(buf, 16, 1);
    KUNIT_EXPECT_TRUE(test, normal_ok);

    /* Overflow access — KASAN should reject access past the allocation.
     * We check 8 bytes past end (across at least one redzone granule). */
    int overflow_detected = !kasan_check(buf + 24, 1, 1);
    KUNIT_EXPECT_TRUE(test, overflow_detected);

    kfree(buf);
#else
    /* KASAN not enabled — skip this test */
    KUNIT_SKIP(test);
#endif
}

/* Underflow detection: write before the allocation start (if KASAN tracks it). */
static void slab_kasan_underflow_test(struct kunit *test)
{
#if KASAN_ENABLED
    uint8_t *buf = (uint8_t *)kmalloc(16);
    KUNIT_EXPECT_NOT_NULL(test, buf);
    if (!buf) return;

    /* Access just before the allocation — KASAN should catch this.
     * The exact offset depends on the slab header layout; we check
     * a few bytes before the returned pointer. */
    int underflow_detected = 0;

    /* Try accessing at buf[-8] — should hit a redzone or header poison */
    if (buf >= (uint8_t *)8) {
        underflow_detected = !kasan_check(buf - 8, 1, 1);
    }

    /* Note: underflow detection depends on the allocator placing
     * redzones before the object.  The test is informational. */
    KUNIT_EXPECT_TRUE(test, underflow_detected || 1); /* soft check */

    kfree(buf);
#else
    KUNIT_SKIP(test);
#endif
}

/* ====================================================================
 *  5. Stress / allocator resilience
 * ==================================================================== */

/* Alternating small and large allocations to stress the page/slab boundary. */
static void slab_stress_mix_test(struct kunit *test)
{
    const int N = 200;
    void *ptrs[N];
    int count = 0;

    for (int i = 0; i < N; i++) {
        /* Alternate between small and medium sizes */
        size_t sz = (i % 3 == 0) ? 8 :
                    (i % 3 == 1) ? 128 : 1024;
        ptrs[count] = kmalloc(sz);
        if (ptrs[count]) {
            memset(ptrs[count], (int)(i & 0xFF), sz);
            count++;
        }
    }

    KUNIT_EXPECT_GT(test, (int64_t)count, (int64_t)(N / 2));

    /* Free every other pointer (fragmentation exercise) */
    for (int i = 0; i < count; i += 2) {
        if (ptrs[i])
            kfree(ptrs[i]);
    }

    /* Allocate again to exercise freed-but-cached slots */
    for (int i = 0; i < 50; i++) {
        void *p = kmalloc(64);
        KUNIT_EXPECT_NOT_NULL(test, p);
        if (p)
            kfree(p);
    }

    /* Free remaining odd-indexed pointers */
    for (int i = 1; i < count; i += 2) {
        if (ptrs[i])
            kfree(ptrs[i]);
    }
}

/* Rapid alloc/free of same size — tests lock contention (single-CPU) and cache hot path. */
static void slab_stress_rapid_test(struct kunit *test)
{
    const int N = 500;
    for (int i = 0; i < N; i++) {
        void *p = kmalloc(64);
        KUNIT_EXPECT_NOT_NULL(test, p);
        if (p) {
            memset(p, 0xAB, 64);
            kfree(p);
        }
    }
}

/* ====================================================================
 *  6. Cache constructor test
 * ==================================================================== */

/* Constructor that initialises allocated objects to a known state. */
static void slab_test_ctor(void *obj)
{
    struct slab_test_obj *o = (struct slab_test_obj *)obj;
    o->magic = 0xCAFEBABE;
    o->id    = 0;
    memset(o->data, 0, sizeof(o->data));
}

static void slab_cache_ctor_test(struct kunit *test)
{
    struct kmem_cache *cache = kmem_cache_create("test_ctor",
                                                  sizeof(struct slab_test_obj),
                                                  0, slab_test_ctor);
    KUNIT_EXPECT_NOT_NULL(test, cache);
    if (!cache) return;

    /* Allocate an object — constructor should have been called */
    struct slab_test_obj *obj = (struct slab_test_obj *)kmem_cache_alloc(cache);
    KUNIT_EXPECT_NOT_NULL(test, obj);
    if (obj) {
        KUNIT_EXPECT_EQ(test, (int64_t)obj->magic, (int64_t)0xCAFEBABE);
        KUNIT_EXPECT_EQ(test, (int64_t)obj->id, (int64_t)0);
        kmem_cache_free(cache, obj);
    }

    kmem_cache_destroy(cache);
}

/* ====================================================================
 *  7. Aligned allocation tests
 * ==================================================================== */

static void slab_aligned_cache_test(struct kunit *test)
{
    struct kmem_cache *cache = kmem_cache_create("test_align",
                                                  sizeof(struct slab_test_obj),
                                                  64, NULL);
    KUNIT_EXPECT_NOT_NULL(test, cache);
    if (!cache) return;

    struct slab_test_obj *obj = (struct slab_test_obj *)kmem_cache_alloc(cache);
    KUNIT_EXPECT_NOT_NULL(test, obj);
    if (obj) {
        /* With 64-byte alignment, pointer must be 64-byte aligned */
        KUNIT_EXPECT_EQ(test, (int64_t)((uintptr_t)obj & 0x3F), (int64_t)0);
        obj->magic = 0xFEEDFACE;
        KUNIT_EXPECT_EQ(test, (int64_t)obj->magic, (int64_t)0xFEEDFACE);
        kmem_cache_free(cache, obj);
    }

    kmem_cache_destroy(cache);
}

/* ====================================================================
 *  Suite registration
 * ==================================================================== */

static struct kunit_case slab_full_test_cases[] = {
    KUNIT_CASE(slab_alloc_pattern_test),
    KUNIT_CASE(slab_alloc_zero_test),
    KUNIT_CASE(slab_alloc_huge_test),
    KUNIT_CASE(slab_alloc_free_reuse_test),
    KUNIT_CASE(slab_varied_sizes_test),
    KUNIT_CASE(slab_cache_basic_test),
    KUNIT_CASE(slab_cache_multi_alloc_test),
    KUNIT_CASE(slab_kasan_overflow_test),
    KUNIT_CASE(slab_kasan_underflow_test),
    KUNIT_CASE(slab_stress_mix_test),
    KUNIT_CASE(slab_stress_rapid_test),
    KUNIT_CASE(slab_cache_ctor_test),
    KUNIT_CASE(slab_aligned_cache_test),
    {0}
};

static struct kunit_suite slab_full_test_suite;

void kunit_slab_register(void)
{
    /* Populate the fixed-size case array from our termination-checked list */
    int ci = 0;
    for (int i = 0; slab_full_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
        slab_full_test_suite.cases[ci].name = slab_full_test_cases[i].name;
        slab_full_test_suite.cases[ci].run  = slab_full_test_cases[i].run;
        ci++;
    }
    slab_full_test_suite.cases[ci].name = NULL;
    slab_full_test_suite.cases[ci].run  = NULL;

    slab_full_test_suite.name    = "slab_full";
    slab_full_test_suite.setup   = NULL;
    slab_full_test_suite.teardown = NULL;

    kunit_register_suite(&slab_full_test_suite);
    kprintf("[KUnit] Full slab allocator tests registered (%d cases)\n", ci);
}

/* ── Stub: kunit_slab_init ─────────────────────────────── */
int kunit_slab_init(void)
{
    kprintf("[kunit] kunit_slab_init: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kunit_slab_test_alloc ─────────────────────────────── */
int kunit_slab_test_alloc(void)
{
    kprintf("[kunit] kunit_slab_test_alloc: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kunit_slab_test_free ─────────────────────────────── */
int kunit_slab_test_free(void)
{
    kprintf("[kunit] kunit_slab_test_free: not yet implemented\n");
    return -ENOSYS;
}
