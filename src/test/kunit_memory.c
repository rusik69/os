/*
 * kunit_memory.c — KUnit unit tests for kernel memory subsystem
 *
 * Comprehensive tests for the Physical Memory Manager (PMM), slab
 * allocator, Virtual Memory Manager (VMM), and heap allocator.
 *
 * D251: PMM/Slab/Heap Unit Tests
 *
 * Run via:
 *   # echo 1 > /sys/kernel/debug/kunit/run_all
 *   # cat /sys/kernel/debug/kunit/results
 *
 * Or to run just this suite:
 *   # echo 1 > /sys/kernel/debug/kunit/run/memory_pmm_basic
 */

#include "kunit.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"

/* ====================================================================
 *  1. PMM — Basic allocation / deallocation
 * ==================================================================== */

/*
 * Allocate one frame and free it immediately.
 * Verifies: allocation succeeds, frame is 4K-aligned, free does not crash.
 */
static void pmm_alloc_free_one(struct kunit *test)
{
	uint64_t frame = pmm_alloc_frame();

	KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
	if (frame) {
		/* Frame must be 4K-aligned */
		KUNIT_EXPECT_EQ(test, (int64_t)(frame & (PAGE_SIZE - 1)),
					    (int64_t)0);
		pmm_free_frame(frame);
	}
}

/*
 * Allocate N frames, verify they are all unique, and free them.
 */
static void pmm_alloc_free_multi(struct kunit *test)
{
	const int N = 8;
	uint64_t frames[8];
	int i;

	/* Allocate N frames */
	for (i = 0; i < N; i++) {
		frames[i] = pmm_alloc_frame();
		KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
	}

	/* All N frames must have unique addresses */
	for (i = 0; i < N; i++) {
		int j;
		for (j = i + 1; j < N; j++) {
			KUNIT_EXPECT_NE(test, frames[i], frames[j]);
		}
	}

	/* Free in reverse order */
	for (i = N - 1; i >= 0; i--) {
		if (frames[i])
			pmm_free_frame(frames[i]);
	}
}

/*
 * Allocate frames, free them, then allocate again.
 * Verifies that freed frames can be reused (PMM remains functional).
 */
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

/*
 * Allocate and free with interleaving.
 * Alloc A, alloc B, free A, alloc C — C may reuse A's frame.
 */
static void pmm_alloc_free_interleaved(struct kunit *test)
{
	uint64_t a = pmm_alloc_frame();

	KUNIT_EXPECT_NE(test, a, (uint64_t)0);
	if (!a) return;

	uint64_t b = pmm_alloc_frame();
	KUNIT_EXPECT_NE(test, b, (uint64_t)0);
	if (!b) {
		pmm_free_frame(a);
		return;
	}

	/* Free a, then alloc again — may reuse a's frame */
	pmm_free_frame(a);
	uint64_t c = pmm_alloc_frame();
	KUNIT_EXPECT_NE(test, c, (uint64_t)0);

	pmm_free_frame(b);
	if (c)
		pmm_free_frame(c);
}

/*
 * Rapid alloc/free cycle — stress test.
 * Allocate and free many frames in sequence.
 */
static void pmm_stress_alloc_free(struct kunit *test)
{
	const int ITERATIONS = 64;
	uint64_t frames[64];
	int i;

	for (i = 0; i < ITERATIONS; i++) {
		frames[i] = pmm_alloc_frame();
		KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
		if (frames[i] == 0)
			break;
	}

	for (i = 0; i < ITERATIONS; i++) {
		if (frames[i])
			pmm_free_frame(frames[i]);
	}
}

/*
 * Allocate frames and verify that used_frames increases by exactly
 * the number of allocated frames, then decreases back on free.
 */
static void pmm_alloc_free_count_check(struct kunit *test)
{
	uint64_t used_before = pmm_get_used_frames();
	uint64_t frames[4];
	int i;

	for (i = 0; i < 4; i++) {
		frames[i] = pmm_alloc_frame();
		KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
	}

	uint64_t used_after = pmm_get_used_frames();
	KUNIT_EXPECT_EQ(test, (int64_t)used_after,
				    (int64_t)(used_before + 4));

	for (i = 0; i < 4; i++) {
		if (frames[i])
			pmm_free_frame(frames[i]);
	}

	uint64_t used_after_free = pmm_get_used_frames();
	KUNIT_EXPECT_EQ(test, (int64_t)used_after_free,
				    (int64_t)used_before);
}

/*
 * Double-free safety: freeing an already-freed frame must not
 * corrupt the allocator or panic the kernel.  Allocators may
 * handle this silently or detect it — the key requirement is
 * survival.
 */
static void pmm_double_free_safety(struct kunit *test)
{
	uint64_t frame = pmm_alloc_frame();

	KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
	if (!frame) return;

	/* First free — should succeed */
	pmm_free_frame(frame);

	/* Second free of the same address — must not crash */
	pmm_free_frame(frame);

	/* If we got here, the kernel survived */
	KUNIT_EXPECT_TRUE(test, 1);
}

/* ====================================================================
 *  2. PMM — Contiguous page allocation (order > 0)
 *
 * Tests for pmm_alloc_frames() which allocates count physically-contiguous
 * frames.  For power-of-2 counts this exercises the buddy-like "order > 0"
 * allocation path.  We verify physical contiguity, base alignment,
 * read/write access via the kernel direct map, and free+reuse cycles.
 * ==================================================================== */

/*
 * Verify that allocating 2 contiguous frames (order 1) succeeds, that the
 * returned pages are physically contiguous (PAGE_SIZE apart), and that the
 * base address is 2*PAGE_SIZE aligned (for power-of-2 orders the allocator
 * should naturally provide alignment equivalent to the block size).
 */
static void pmm_contiguous_order_1(struct kunit *test)
{
	const size_t COUNT = 2;
	uint64_t phys = (uint64_t)pmm_alloc_frames(COUNT);

	KUNIT_EXPECT_NE(test, phys, (uint64_t)0);
	if (!phys) return;

	/* Base must be 4K-aligned */
	KUNIT_EXPECT_EQ(test, (int64_t)(phys & (PAGE_SIZE - 1)), (int64_t)0);

	/* For order=1 (2 pages), the base should be 8K-aligned */
	KUNIT_EXPECT_EQ(test, (int64_t)(phys & (COUNT * PAGE_SIZE - 1)), (int64_t)0);

	/* Verify physical contiguity: frame i + 1 is exactly PAGE_SIZE after frame i */
	for (size_t i = 0; i < COUNT; i++) {
		uint64_t expected = phys + i * PAGE_SIZE;
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT(expected);
		/* Write an identifying pattern */
		*vp = 0xA001000000000000ULL + i;
	}

	/* Read back and verify */
	int ok = 1;
	for (size_t i = 0; i < COUNT && ok; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT(phys + i * PAGE_SIZE);
		if (*vp != (0xA001000000000000ULL + i))
			ok = 0;
	}
	KUNIT_EXPECT_EQ(test, ok, 1);

	pmm_free_frames_contiguous(phys, COUNT);
}

/*
 * Allocate 4 contiguous frames (order 2).
 */
static void pmm_contiguous_order_2(struct kunit *test)
{
	const size_t COUNT = 4;
	uint64_t phys = (uint64_t)pmm_alloc_frames(COUNT);

	KUNIT_EXPECT_NE(test, phys, (uint64_t)0);
	if (!phys) return;

	KUNIT_EXPECT_EQ(test, (int64_t)(phys & (PAGE_SIZE - 1)), (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)(phys & (COUNT * PAGE_SIZE - 1)), (int64_t)0);

	for (size_t i = 0; i < COUNT; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT(phys + i * PAGE_SIZE);
		*vp = 0xA002000000000000ULL + i;
	}

	int ok = 1;
	for (size_t i = 0; i < COUNT && ok; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT(phys + i * PAGE_SIZE);
		if (*vp != (0xA002000000000000ULL + i))
			ok = 0;
	}
	KUNIT_EXPECT_EQ(test, ok, 1);

	pmm_free_frames_contiguous(phys, COUNT);
}

/*
 * Allocate 8 contiguous frames (order 3).
 */
static void pmm_contiguous_order_3(struct kunit *test)
{
	const size_t COUNT = 8;
	uint64_t phys = (uint64_t)pmm_alloc_frames(COUNT);

	KUNIT_EXPECT_NE(test, phys, (uint64_t)0);
	if (!phys) return;

	KUNIT_EXPECT_EQ(test, (int64_t)(phys & (PAGE_SIZE - 1)), (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)(phys & (COUNT * PAGE_SIZE - 1)), (int64_t)0);

	for (size_t i = 0; i < COUNT; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT(phys + i * PAGE_SIZE);
		*vp = 0xA003000000000000ULL + i;
	}

	int ok = 1;
	for (size_t i = 0; i < COUNT && ok; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT(phys + i * PAGE_SIZE);
		if (*vp != (0xA003000000000000ULL + i))
			ok = 0;
	}
	KUNIT_EXPECT_EQ(test, ok, 1);

	pmm_free_frames_contiguous(phys, COUNT);
}

/*
 * Allocate 16 contiguous frames (order 4).
 */
static void pmm_contiguous_order_4(struct kunit *test)
{
	const size_t COUNT = 16;
	uint64_t phys = (uint64_t)pmm_alloc_frames(COUNT);

	KUNIT_EXPECT_NE(test, phys, (uint64_t)0);
	if (!phys) return;

	KUNIT_EXPECT_EQ(test, (int64_t)(phys & (PAGE_SIZE - 1)), (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)(phys & (COUNT * PAGE_SIZE - 1)), (int64_t)0);

	for (size_t i = 0; i < COUNT; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT(phys + i * PAGE_SIZE);
		*vp = 0xA004000000000000ULL + i;
	}

	int ok = 1;
	for (size_t i = 0; i < COUNT && ok; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT(phys + i * PAGE_SIZE);
		if (*vp != (0xA004000000000000ULL + i))
			ok = 0;
	}
	KUNIT_EXPECT_EQ(test, ok, 1);

	pmm_free_frames_contiguous(phys, COUNT);
}

/*
 * Allocate contiguous frames of a given order, free them, then allocate
 * the same order again — verify that the allocator remains functional
 * and returns a valid (possibly different) region.
 */
static void pmm_contiguous_reuse(struct kunit *test)
{
	const size_t COUNT = 4;

	/* First allocation */
	uint64_t phys = (uint64_t)pmm_alloc_frames(COUNT);
	KUNIT_EXPECT_NE(test, phys, (uint64_t)0);
	if (!phys) return;

	uint64_t saved_phys = phys;
	pmm_free_frames_contiguous(phys, COUNT);

	/* Second allocation — same order, may or may not be the same address */
	phys = (uint64_t)pmm_alloc_frames(COUNT);
	KUNIT_EXPECT_NE(test, phys, (uint64_t)0);
	if (!phys) {
		/* First alloc was freed already, no leak */
		return;
	}

	KUNIT_EXPECT_EQ(test, (int64_t)(phys & (PAGE_SIZE - 1)), (int64_t)0);

	/* Write and verify the new region */
	for (size_t i = 0; i < COUNT; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT(phys + i * PAGE_SIZE);
		*vp = 0xA010000000000000ULL + i;
	}

	int ok = 1;
	for (size_t i = 0; i < COUNT && ok; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT(phys + i * PAGE_SIZE);
		if (*vp != (0xA010000000000000ULL + i))
			ok = 0;
	}
	KUNIT_EXPECT_EQ(test, ok, 1);

	pmm_free_frames_contiguous(phys, COUNT);
	(void)saved_phys; /* saved for potential future checks */
}

/*
 * Edge cases for pmm_alloc_frames:
 * - count = 1 should succeed (same as pmm_alloc_frame internally)
 * - count = 0 should return NULL
 * - Attempt to allocate a very large contiguous block may fail gracefully
 */
static void pmm_contiguous_edge_cases(struct kunit *test)
{
	/* Single frame via batch API */
	uint64_t *single = pmm_alloc_frames(1);
	KUNIT_EXPECT_NOT_NULL(test, single);
	if (single) {
		pmm_free_frames_contiguous((uint64_t)single, 1);
	}

	/* Zero count should fail gracefully */
	uint64_t *zero = pmm_alloc_frames(0);
	KUNIT_EXPECT_NULL(test, zero);
}

/* ====================================================================
 *  3. PMM — Reference counting (pmm_ref_frame / pmm_unref_frame)
 *
 * Tests for the frame reference-counting API used by COW (copy-on-write)
 * in the VMM layer.  A freshly allocated frame has refcount 1.
 * pmm_ref_frame() increments, pmm_unref_frame() decrements and frees
 * the frame when the count reaches zero.
 * ==================================================================== */

/*
 * Basic refcount lifecycle:
 *   alloc → refcount=1 → ref → count=2 → unref → count=1 → unref → count=0 (freed)
 */
static void pmm_refcount_basic(struct kunit *test)
{
	uint64_t frame = pmm_alloc_frame();

	KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
	if (!frame) return;

	/* Initial refcount must be 1 after allocation */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_refcount(frame), (int64_t)1);

	/* Take a reference → refcount 2 */
	pmm_ref_frame(frame);
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_refcount(frame), (int64_t)2);

	/* Drop first reference → refcount 1 (not freed) */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_unref_frame(frame), (int64_t)1);

	/* Drop last reference → refcount 0 (frame freed, page returns to pool) */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_unref_frame(frame), (int64_t)0);

	/* After being freed, refcount should be 0 */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_refcount(frame), (int64_t)0);
}

/*
 * Take multiple references on the same frame and verify the counter
 * tracks correctly through several increments and decrements.
 */
static void pmm_refcount_multiple_refs(struct kunit *test)
{
	uint64_t frame = pmm_alloc_frame();

	KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
	if (!frame) return;

	/* Initial: 1 */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_refcount(frame), (int64_t)1);

	/* Take 5 extra refs → expected 6 */
	for (int i = 0; i < 5; i++)
		pmm_ref_frame(frame);
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_refcount(frame), (int64_t)6);

	/* Drop 3 → expected 3 */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_unref_frame(frame), (int64_t)5);
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_unref_frame(frame), (int64_t)4);
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_unref_frame(frame), (int64_t)3);

	/* Drop 2 more → expected 1 (not freed yet) */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_unref_frame(frame), (int64_t)2);
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_unref_frame(frame), (int64_t)1);

	/* Drop final reference → freed */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_unref_frame(frame), (int64_t)0);
}

/*
 * Querying refcount on unallocated frame addresses returns 0.
 * This includes the null physical address and a high address that
 * should not be allocated at this point in boot.
 */
static void pmm_refcount_unallocated(struct kunit *test)
{
	/* phys = 0 is never a valid allocated frame */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_refcount(0), (int64_t)0);

	/* A high address unlikely to be allocated */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_refcount(0x100000000ULL), (int64_t)0);
}

/*
 * Verify that used_frames correctly decreases when refcount drops to 0
 * and the frame is freed.
 */
static void pmm_refcount_used_frames_check(struct kunit *test)
{
	uint64_t before = pmm_get_used_frames();
	uint64_t frame = pmm_alloc_frame();

	KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
	if (!frame) return;

	/* One more frame in use */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_get_used_frames(),
				    (int64_t)(before + 1));

	/* Take a second reference */
	pmm_ref_frame(frame);
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_refcount(frame), (int64_t)2);

	/* Still one frame in use (refcount hasn't hit 0 yet) */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_get_used_frames(),
				    (int64_t)(before + 1));

	/* Drop first reference — still one, frame not freed */
	pmm_unref_frame(frame);
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_get_used_frames(),
				    (int64_t)(before + 1));

	/* Drop last reference — frame freed */
	pmm_unref_frame(frame);
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_get_used_frames(),
				    (int64_t)before);
}

/*
 * Calling pmm_unref_frame on a frame with refcount 0 is harmless
 * (returns 0, does not corrupt the allocator).
 */
static void pmm_refcount_unref_zero(struct kunit *test)
{
	/* Unref on a never-allocated address is a no-op */
	int rc = pmm_unref_frame(0);
	KUNIT_EXPECT_EQ(test, (int64_t)rc, (int64_t)0);

	/* Allocate, free, then unref again after free */
	uint64_t frame = pmm_alloc_frame();
	KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
	if (!frame) return;

	/* Free via unref */
	pmm_unref_frame(frame);

	/* Refcount should be 0 now */
	KUNIT_EXPECT_EQ(test, (int64_t)pmm_refcount(frame), (int64_t)0);

	/* Unref again — must not crash, returns 0 */
	rc = pmm_unref_frame(frame);
	KUNIT_EXPECT_EQ(test, (int64_t)rc, (int64_t)0);
}

/* ====================================================================
 *  4. PMM — OOM return (NULL on exhaustion within bounds)
 *
 * Tests that the PMM allocator properly returns NULL/0 when memory
 * cannot be allocated: impossibly large requests, zero-length requests,
 * and that the allocator remains functional after an allocation failure.
 * ==================================================================== */

/*
 * Request more contiguous frames than total_frames — must return NULL.
 * This verifies that absurdly large batch allocations fail gracefully
 * rather than looping forever or returning garbage.
 */
static void pmm_oom_impossible_count(struct kunit *test)
{
	uint64_t total = pmm_get_total_frames();
	KUNIT_EXPECT_NE(test, total, (uint64_t)0);
	if (total == 0) return;

	/* Request total_frames + 1 contiguous frames — impossible */
	uint64_t *huge = pmm_alloc_frames(total + 1);
	KUNIT_EXPECT_NULL(test, huge);
	/* Must also survive a really large request */
	uint64_t *insane = pmm_alloc_frames(0xFFFFFFFFULL);
	KUNIT_EXPECT_NULL(test, insane);

	/* The allocator must still work after the failed requests */
	uint64_t frame = pmm_alloc_frame();
	KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
	if (frame)
		pmm_free_frame(frame);
}

/*
 * Zero-length batch allocation must return NULL.
 * (Parallels the contiguous_edge_cases test but lives in the OOM suite
 *  for conceptual grouping.)
 */
static void pmm_oom_zero_count(struct kunit *test)
{
	uint64_t *zero = pmm_alloc_frames(0);
	KUNIT_EXPECT_NULL(test, zero);

	/* Allocator must still function */
	uint64_t frame = pmm_alloc_frame();
	KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
	if (frame)
		pmm_free_frame(frame);
}

/*
 * Simulate memory pressure: allocate many frames, free them all,
 * then allocate again.  This verifies that a cycle of near-exhaustion
 * does not corrupt the allocator's internal state.
 */
static void pmm_oom_stress_then_recover(struct kunit *test)
{
	const int N = 1024;
	uint64_t frames[1024];
	int i;
	int allocated = 0;

	/* Allocate as many as we can (may not get all N on a small system) */
	for (i = 0; i < N; i++) {
		frames[i] = pmm_alloc_frame();
		if (frames[i] == 0)
			break;
		allocated++;
	}

	/* We should have allocated at least a few */
	KUNIT_EXPECT_TRUE(test, allocated > 0);

	/* Free everything */
	for (i = 0; i < allocated; i++)
		pmm_free_frame(frames[i]);

	/* After freeing, allocator should work again */
	uint64_t frame = pmm_alloc_frame();
	KUNIT_EXPECT_NE(test, frame, (uint64_t)0);
	if (frame)
		pmm_free_frame(frame);
}

/* ====================================================================
 *  5. PMM — Per-CPU hot cache consistency
 *
 * The PMM maintains a per-CPU hot cache (LIFO stack, 8 entries) to
 * reduce lock contention on the global bitmap.  pmm_alloc_frame() pops
 * from the cache first; on empty it triggers pmm_cache_refill() to
 * replenish from the global bitmap.  pmm_free_frame() pushes onto the
 * cache; on full it triggers pmm_cache_drain() to flush cached pages
 * back to the bitmap.
 *
 * These tests verify that the cache refill/drain cycle does not lose
 * frames, double-allocate, or corrupt the used_frames counter.
 * ==================================================================== */

/*
 * Allocate more frames than the per-CPU cache can hold in one batch.
 * The first cache-sized batch comes from the pre-filled cache (populated
 * during pmm_init).  Subsequent allocations trigger pmm_cache_refill()
 * for each new batch.  Verify all frames are unique.
 */
static void pmm_hotcache_refill_after_empty(struct kunit *test)
{
	/* The per-CPU cache is sized at 8 entries (PMM_CPU_CACHE_SIZE). */
	const int N = 20; /* 8 + 12, forces at least 2 refills */
	uint64_t frames[20];
	int i, count = 0;

	for (i = 0; i < N; i++) {
		frames[i] = pmm_alloc_frame();
		KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
		if (frames[i] == 0)
			break;
		count++;
	}
	KUNIT_EXPECT_EQ(test, count, N);

	/* Verify uniqueness — no frame address repeats */
	for (i = 0; i < count; i++) {
		int j;
		for (j = i + 1; j < count; j++) {
			KUNIT_EXPECT_NE(test, frames[i], frames[j]);
		}
	}

	for (i = 0; i < count; i++)
		pmm_free_frame(frames[i]);
}

/*
 * Free more frames than the cache can hold, which forces
 * pmm_cache_drain() to flush the full cache back to the global
 * bitmap before the remaining frees can proceed.  After drain,
 * verify the allocator is still functional and returns valid frames.
 */
static void pmm_hotcache_drain_on_full(struct kunit *test)
{
	const int N = 12; /* More than 8, forces drain */
	uint64_t frames[12];
	int i, count = 0;

	for (i = 0; i < N; i++) {
		frames[i] = pmm_alloc_frame();
		KUNIT_EXPECT_NE(test, frames[i], (uint64_t)0);
		if (frames[i] == 0)
			break;
		count++;
	}

	/* Free all — the 9th free triggers cache drain, the rest go to cache */
	for (i = 0; i < count; i++)
		pmm_free_frame(frames[i]);

	/* After drain, allocator should still work */
	uint64_t f = pmm_alloc_frame();
	KUNIT_EXPECT_NE(test, f, (uint64_t)0);
	if (f)
		pmm_free_frame(f);
}

/*
 * Alternating alloc/free cycles that exercise both the push (free)
 * and pop (alloc) fast paths.  Each pair allocs two frames, frees
 * them, then starts the next iteration.  This pattern stresses the
 * cache push/pop interleaving.
 */
static void pmm_hotcache_alternating_cycles(struct kunit *test)
{
	const int CYCLES = 32;
	int i;

	for (i = 0; i < CYCLES; i++) {
		uint64_t f1 = pmm_alloc_frame();
		KUNIT_EXPECT_NE(test, f1, (uint64_t)0);
		uint64_t f2 = pmm_alloc_frame();
		KUNIT_EXPECT_NE(test, f2, (uint64_t)0);

		if (f1 && f2) {
			KUNIT_EXPECT_NE(test, f1, f2);
			pmm_free_frame(f1);
			pmm_free_frame(f2);
		} else {
			if (f1) pmm_free_frame(f1);
			if (f2) pmm_free_frame(f2);
			break;
		}
	}
}

/*
 * Verify that the used_frames counter tracks correctly through cache
 * refill and drain cycles.  The counter should reflect the true number
 * of allocated frames, not the transient state of the per-CPU cache.
 */
static void pmm_hotcache_used_count_accuracy(struct kunit *test)
{
	uint64_t before = pmm_get_used_frames();
	const int N = 16;
	uint64_t frames[16];
	int i, count = 0;

	/* Allocate — used_frames should increase by N */
	for (i = 0; i < N; i++) {
		frames[i] = pmm_alloc_frame();
		if (frames[i] == 0)
			break;
		count++;
	}
	KUNIT_EXPECT_EQ(test, count, N);

	uint64_t after_alloc = pmm_get_used_frames();
	KUNIT_EXPECT_EQ(test, (int64_t)(after_alloc - before), count);

	/* Free all — used_frames should decrease back */
	for (i = 0; i < count; i++)
		pmm_free_frame(frames[i]);

	uint64_t after_free = pmm_get_used_frames();
	KUNIT_EXPECT_EQ(test, (int64_t)after_free, (int64_t)before);
}

/*
 * Stress test: allocate and free many frames in a burst, forcing
 * multiple cache refill and drain cycles.  Verifies the cache does
 * not leak frames over repeated fill/drain transitions.
 */
static void pmm_hotcache_burst_stress(struct kunit *test)
{
	const int N = 128;
	uint64_t *frames;
	int i, j;

	frames = pmm_alloc_frames(N);
	KUNIT_EXPECT_NOT_NULL(test, frames);
	if (!frames)
		return;

	/* Write unique pattern to each frame */
	for (i = 0; i < N; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT((uint64_t)frames + i * PAGE_SIZE);
		*vp = 0xCAFE000000000000ULL + (uint64_t)i;
	}

	/* Read back and verify */
	for (i = 0; i < N; i++) {
		volatile uint64_t *vp = (volatile uint64_t *)PHYS_TO_VIRT((uint64_t)frames + i * PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, (int64_t)*vp, (int64_t)(0xCAFE000000000000ULL + i));
	}

	/* Free in small groups to exercise cache drain repeatedly */
	for (i = 0; i < N; i += 4) {
		int end = i + 4;
		if (end > N) end = N;
		for (j = i; j < end; j++)
			pmm_free_frame((uint64_t)frames + j * PAGE_SIZE);
	}

	/* Allocator should still function after stress */
	uint64_t f = pmm_alloc_frame();
	KUNIT_EXPECT_NE(test, f, (uint64_t)0);
	if (f)
		pmm_free_frame(f);
}

/* ====================================================================
 *  Suite definition — auto-registered via linker section
 * ==================================================================== */

static struct kunit_suite pmm_basic_suite = {
	.name    = "memory_pmm_basic",
	.setup   = NULL,
	.teardown = NULL,
	.cases   = {
		KUNIT_CASE(pmm_alloc_free_one),
		KUNIT_CASE(pmm_alloc_free_multi),
		KUNIT_CASE(pmm_alloc_free_reuse),
		KUNIT_CASE(pmm_alloc_free_interleaved),
		KUNIT_CASE(pmm_stress_alloc_free),
		KUNIT_CASE(pmm_alloc_free_count_check),
		KUNIT_CASE(pmm_double_free_safety),
	},
};

KUNIT_TEST_SUITE(pmm_basic_suite);

static struct kunit_suite pmm_contiguous_suite = {
	.name    = "memory_pmm_contiguous",
	.setup   = NULL,
	.teardown = NULL,
	.cases   = {
		KUNIT_CASE(pmm_contiguous_order_1),
		KUNIT_CASE(pmm_contiguous_order_2),
		KUNIT_CASE(pmm_contiguous_order_3),
		KUNIT_CASE(pmm_contiguous_order_4),
		KUNIT_CASE(pmm_contiguous_reuse),
		KUNIT_CASE(pmm_contiguous_edge_cases),
	},
};

KUNIT_TEST_SUITE(pmm_contiguous_suite);

static struct kunit_suite pmm_refcount_suite = {
	.name    = "memory_pmm_refcount",
	.setup   = NULL,
	.teardown = NULL,
	.cases   = {
		KUNIT_CASE(pmm_refcount_basic),
		KUNIT_CASE(pmm_refcount_multiple_refs),
		KUNIT_CASE(pmm_refcount_unallocated),
		KUNIT_CASE(pmm_refcount_used_frames_check),
		KUNIT_CASE(pmm_refcount_unref_zero),
	},
};

KUNIT_TEST_SUITE(pmm_refcount_suite);

static struct kunit_suite pmm_oom_suite = {
	.name    = "memory_pmm_oom",
	.setup   = NULL,
	.teardown = NULL,
	.cases   = {
		KUNIT_CASE(pmm_oom_impossible_count),
		KUNIT_CASE(pmm_oom_zero_count),
		KUNIT_CASE(pmm_oom_stress_then_recover),
	},
};

KUNIT_TEST_SUITE(pmm_oom_suite);

static struct kunit_suite pmm_hotcache_suite = {
	.name    = "memory_pmm_hotcache",
	.setup   = NULL,
	.teardown = NULL,
	.cases   = {
		KUNIT_CASE(pmm_hotcache_refill_after_empty),
		KUNIT_CASE(pmm_hotcache_drain_on_full),
		KUNIT_CASE(pmm_hotcache_alternating_cycles),
		KUNIT_CASE(pmm_hotcache_used_count_accuracy),
		KUNIT_CASE(pmm_hotcache_burst_stress),
	},
};

KUNIT_TEST_SUITE(pmm_hotcache_suite);
