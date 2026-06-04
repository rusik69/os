/*
 * kunit_pmm.c — KUnit unit tests for the Physical Memory Manager (PMM)
 *
 * Comprehensive tests for frame allocation, freeing, reference counting,
 * migration-type aware allocation, and fragmentation introspection.
 *
 * Item 267: KUnit — PMM allocation tests
 *
 * Run via:
 *   # echo 1 > /sys/kernel/debug/kunit/run_all
 *   # cat /sys/kernel/debug/kunit/results
 *
 * Or to run just this suite:
 *   # echo 1 > /sys/kernel/debug/kunit/run/pmm
 */

#include "kunit.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"

/* ====================================================================
 *  1. Basic allocation / deallocation
 * ==================================================================== */

/* Allocate one frame and free it immediately. */
static void pmm_alloc_free_one(struct kunit *test)
{
    uint64_t frame = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
    if (frame) {
        /* Frame must be 4K-aligned */
        KUNIT_EXPECT_EQ(test, (int64_t)(frame & (PAGE_SIZE - 1)), (int64_t)0);
        pmm_free_frame(frame);
    }
}

/* Allocate multiple frames and free them all. */
static void pmm_alloc_free_multi(struct kunit *test)
{
    const int N = 8;
    uint64_t frames[N];
    int i;

    /* Allocate N frames */
    for (i = 0; i < N; i++) {
        frames[i] = pmm_alloc_frame();
        KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
    }

    /* All N frames must have unique addresses */
    for (i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            KUNIT_EXPECT_NE(test, frames[i], frames[j]);
        }
    }

    /* Free in reverse order */
    for (i = N - 1; i >= 0; i--) {
        if (frames[i])
            pmm_free_frame(frames[i]);
    }
}

/* Allocate frames, free them, then allocate again — verify reuse works. */
static void pmm_alloc_free_reuse(struct kunit *test)
{
    uint64_t frames[4];
    uint64_t frames2[4];
    int i;

    for (i = 0; i < 4; i++) {
        frames[i] = pmm_alloc_frame();
        KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
    }

    for (i = 0; i < 4; i++) {
        if (frames[i])
            pmm_free_frame(frames[i]);
    }

    /* Allocate again — should succeed */
    for (i = 0; i < 4; i++) {
        frames2[i] = pmm_alloc_frame();
        KUNIT_EXPECT_NE(test, frames2[i], (uint64_t)0);
    }

    for (i = 0; i < 4; i++) {
        if (frames2[i])
            pmm_free_frame(frames2[i]);
    }
}

/* ====================================================================
 *  2. Reference counting tests
 * ==================================================================== */

static void pmm_refcount_basic(struct kunit *test)
{
    uint64_t frame = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
    if (!frame) return;

    /* Initial refcount should be 1 (from allocation) */
    int rc = pmm_refcount(frame);
    KUNIT_EXPECT_EQ(test, (int64_t)rc, (int64_t)1);

    /* Take a reference */
    pmm_ref_frame(frame);
    rc = pmm_refcount(frame);
    KUNIT_EXPECT_EQ(test, (int64_t)rc, (int64_t)2);

    /* Release first reference (refcount 2→1, should not free) */
    rc = pmm_unref_frame(frame);
    KUNIT_EXPECT_EQ(test, (int64_t)rc, (int64_t)1);

    /* Release second reference (refcount 1→0, should free) */
    rc = pmm_unref_frame(frame);
    KUNIT_EXPECT_EQ(test, (int64_t)rc, (int64_t)0);
}

/* Refcount on an unallocated frame should return 0 */
static void pmm_refcount_unallocated(struct kunit *test)
{
    int rc = pmm_refcount(0);
    KUNIT_EXPECT_EQ(test, (int64_t)rc, (int64_t)0);

    /* A high physical address unlikely to be allocated should be 0 */
    rc = pmm_refcount(0x100000000ULL);
    KUNIT_EXPECT_EQ(test, (int64_t)rc, (int64_t)0);
}

/* ====================================================================
 *  3. Batch allocation (pmm_alloc_frames / pmm_free_frames_contiguous)
 * ==================================================================== */

static void pmm_batch_alloc_free(struct kunit *test)
{
    const size_t BATCH = 16;
    uint64_t *frames = pmm_alloc_frames(BATCH);
    KUNIT_EXPECT_NOT_NULL(test, frames);
    if (!frames) return;

    for (size_t i = 0; i < BATCH; i++) {
        KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
    }

    /* Free contiguous batch — use the first frame address */
    pmm_free_frames_contiguous(frames[0], BATCH);
}

/* ====================================================================
 *  4. Migration-type aware allocation (Item 121)
 * ==================================================================== */

static void pmm_migrate_alloc(struct kunit *test)
{
    uint64_t frame;

    /* Allocate from MOVABLE type */
    frame = pmm_alloc_frame_migrate(MIGRATE_MOVABLE);
    KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
    if (frame) {
        pmm_free_frame(frame);
    }

    /* Allocate from UNMOVABLE type */
    frame = pmm_alloc_frame_migrate(MIGRATE_UNMOVABLE);
    KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
    if (frame) {
        pmm_free_frame(frame);
    }

    /* Allocate from RECLAIMABLE type */
    frame = pmm_alloc_frame_migrate(MIGRATE_RECLAIMABLE);
    KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
    if (frame) {
        pmm_free_frame(frame);
    }
}

/* ====================================================================
 *  5. Zeroed-page verification
 * ==================================================================== */

static void pmm_alloc_zeroed(struct kunit *test)
{
    uint64_t frame = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
    if (!frame) return;

    /* Map via PHYS_TO_VIRT and check the page is zeroed */
    volatile uint64_t *ptr = (volatile uint64_t *)(uintptr_t)PHYS_TO_VIRT(frame);

    int is_zero = 1;
    for (size_t i = 0; i < (PAGE_SIZE / sizeof(uint64_t)); i++) {
        if (ptr[i] != 0) {
            is_zero = 0;
            break;
        }
    }
    KUNIT_EXPECT_EQ(test, is_zero, 1);

    pmm_free_frame(frame);
}

/* ====================================================================
 *  6. Stress test — rapid alloc/free cycle
 * ==================================================================== */

static void pmm_stress_alloc_free(struct kunit *test)
{
    const int ITERATIONS = 64;
    uint64_t frames[ITERATIONS];

    for (int i = 0; i < ITERATIONS; i++) {
        frames[i] = pmm_alloc_frame();
        KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
        if (frames[i] == 0)
            break;
    }

    for (int i = 0; i < ITERATIONS; i++) {
        if (frames[i])
            pmm_free_frame(frames[i]);
    }
}

/* ====================================================================
 *  7. Fragmentation introspection
 * ==================================================================== */

static void pmm_fragmentation_stats(struct kunit *test)
{
    /* Before any operation, stats should be consistent */
    uint64_t total = pmm_get_total_frames();
    uint64_t used  = pmm_get_used_frames();
    uint64_t largest = pmm_largest_free_block();
    uint64_t blocks  = pmm_free_block_count();

    /* Sanity: total must be >= used (there may be reserved frames, so total > used) */
    KUNIT_EXPECT_NE(test, total, (uint64_t)0);
    KUNIT_EXPECT_NE(test, largest, (uint64_t)0);
    KUNIT_EXPECT_NE(test, blocks, (uint64_t)0);

    /* Allocate a frame and check used_frames increased */
    uint64_t frame = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
    if (frame) {
        uint64_t used_after = pmm_get_used_frames();
        KUNIT_EXPECT_EQ(test, (int64_t)used_after, (int64_t)(used + 1));

        pmm_free_frame(frame);
        uint64_t used_after_free = pmm_get_used_frames();
        KUNIT_EXPECT_EQ(test, (int64_t)used_after_free, (int64_t)used);
    }
}

/* ====================================================================
 *  8. pmm_dump_stats — must not crash
 * ==================================================================== */

static void pmm_dump_stats_smoke(struct kunit *test)
{
    /* Dumping stats should not crash or produce obviously bogus output.
     * We just verify it runs without faulting. */
    pmm_dump_stats();

    /* If we got here, it didn't crash — that's a pass. */
    KUNIT_EXPECT_TRUE(test, 1);
}

/* ====================================================================
 *  9. Rapid alloc / free with interleaving
 * ==================================================================== */

static void pmm_interleaved_alloc_free(struct kunit *test)
{
    uint64_t a = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, a, (uint64_t)0);
    if (!a) return;

    uint64_t b = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, b, (uint64_t)0);
    if (!b) { pmm_free_frame(a); return; }

    /* Free a, then alloc again — the allocator may reuse a's frame */
    pmm_free_frame(a);
    uint64_t c = pmm_alloc_frame();
    KUNIT_EXPECT_NE(test, c, (uint64_t)0);

    pmm_free_frame(b);
    if (c) pmm_free_frame(c);
}

/* ====================================================================
 *  10. Frame alignment check
 * ==================================================================== */

static void pmm_frame_alignment(struct kunit *test)
{
    /* Verify that all allocated frames are properly aligned */
    uint64_t frames[4];
    int i;

    for (i = 0; i < 4; i++) {
        frames[i] = pmm_alloc_frame();
        KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
        if (frames[i]) {
            KUNIT_EXPECT_EQ(test, (int64_t)(frames[i] & (PAGE_SIZE - 1)), (int64_t)0);
        }
    }

    for (i = 0; i < 4; i++) {
        if (frames[i])
            pmm_free_frame(frames[i]);
    }
}

/* ====================================================================
 *  Suite definition
 * ==================================================================== */

static struct kunit_case pmm_test_cases[] = {
    KUNIT_CASE(pmm_alloc_free_one),
    KUNIT_CASE(pmm_alloc_free_multi),
    KUNIT_CASE(pmm_alloc_free_reuse),
    KUNIT_CASE(pmm_refcount_basic),
    KUNIT_CASE(pmm_refcount_unallocated),
    KUNIT_CASE(pmm_batch_alloc_free),
    KUNIT_CASE(pmm_migrate_alloc),
    KUNIT_CASE(pmm_alloc_zeroed),
    KUNIT_CASE(pmm_stress_alloc_free),
    KUNIT_CASE(pmm_fragmentation_stats),
    KUNIT_CASE(pmm_dump_stats_smoke),
    KUNIT_CASE(pmm_interleaved_alloc_free),
    KUNIT_CASE(pmm_frame_alignment),
    {NULL, NULL}
};

static struct kunit_suite pmm_test_suite;

void kunit_pmm_register(void)
{
    /* Populate the fixed-size case array */
    int ci = 0;
    for (int i = 0; pmm_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
        pmm_test_suite.cases[ci].name = pmm_test_cases[i].name;
        pmm_test_suite.cases[ci].run  = pmm_test_cases[i].run;
        ci++;
    }
    pmm_test_suite.cases[ci].name = NULL;
    pmm_test_suite.cases[ci].run  = NULL;

    pmm_test_suite.name    = "pmm";
    pmm_test_suite.setup   = NULL;
    pmm_test_suite.teardown = NULL;

    kunit_register_suite(&pmm_test_suite);
    kprintf("[KUnit] PMM allocation tests registered (13 cases)\\n");
}
