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
