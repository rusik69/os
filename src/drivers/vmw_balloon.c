/* ── Module info ────────────────────────────────────────────────────── */
#ifdef MODULE
MODULE_AUTHOR("VMware, Inc.");
MODULE_DESCRIPTION("VMware balloon — MMIO, stats");
MODULE_VERSION("1.0");
#endif

#include "types.h"
#include "errno.h"
#include "printf.h"

/* ── VMware balloon protocol constants ─────────────────────────────── */

/* Maximum number of memory statistics descriptors supported. */
#define VMW_BALLOON_STATS_MAX     32

/* Memory statistics tag IDs (VMware balloon protocol, §3.2).
 *
 * These identify which memory metric a stats descriptor carries.
 * The hypervisor writes an array of tag-value pairs into a shared
 * page; the guest iterates them to build a memory-usage snapshot.
 * A descriptor with tag == 0 and value == 0 is the end-of-list
 * sentinel and MUST NOT be consumed as a real statistic.
 */
#define VMW_BALLOON_STAT_SWAP_IN      0
#define VMW_BALLOON_STAT_SWAP_OUT     1
#define VMW_BALLOON_STAT_MAJFLT       2
#define VMW_BALLOON_STAT_MINFLT       3
#define VMW_BALLOON_STAT_MEMFREE      4
#define VMW_BALLOON_STAT_MEMTOT       5
#define VMW_BALLOON_STAT_AVAIL        6
#define VMW_BALLOON_STAT_CACHES       7
#define VMW_BALLOON_STAT_HTLB_PGALLOC 8
#define VMW_BALLOON_STAT_HTLB_PGFAIL  9
#define VMW_BALLOON_STAT_NR           10  /* number of known tags */

/* ── VMware balloon stats descriptor ───────────────────────────────── */

/**
 * struct vmw_balloon_stats_desc - A single memory statistics descriptor
 *                                 exchanged with the VMware hypervisor.
 * @tag:   Statistic identifier (one of VMW_BALLOON_STAT_*).
 * @value: Statistic value.
 *
 * The hypervisor writes an array of these descriptors into a shared
 * memory page.  A descriptor with tag == 0 and value == 0 marks the
 * end of the list (the sentinel descriptor).  Every non-sentinel
 * descriptor MUST be validated before its tag and value are consumed
 * by the guest, because a malicious or buggy hypervisor could set an
 * out-of-range tag that would cause an out-of-bounds array lookup or
 * incorrect memory accounting on the guest side.
 */
struct vmw_balloon_stats_desc {
	uint16_t tag;       /* statistic identifier                  */
	uint64_t value;     /* statistic value (count or bytes)     */
};

/* ── VMware balloon device state ────────────────────────────────────── */

static struct {
	int present;
	/* Stats descriptor array (shared with the hypervisor). */
	struct vmw_balloon_stats_desc stats_desc[VMW_BALLOON_STATS_MAX];
	int stats_count;
} vmw_balloon;

/* ── Stats descriptor validation ───────────────────────────────────── */

/**
 * vmw_balloon_validate_stats_desc - Validate a stats descriptor before
 *                                   reading its value.
 * @desc: Pointer to the stats descriptor to validate.
 *
 * In the VMware balloon protocol the hypervisor writes TLV-style
 * statistic descriptors into a shared page.  A malformed descriptor
 * (NULL pointer, unknown tag) could cause out-of-bounds reads or
 * incorrect accounting if consumed without validation.
 *
 * This function checks:
 *   - @desc is not NULL.
 *   - @desc->tag is a known statistic identifier (< VMW_BALLOON_STAT_NR).
 *
 * The sentinel descriptor (tag == 0, value == 0) is NOT validated
 * by this function — callers must check for the sentinel separately.
 *
 * Returns 0 if the descriptor is valid, -EINVAL otherwise.
 */
static int
vmw_balloon_validate_stats_desc(const struct vmw_balloon_stats_desc *desc)
{
	/* NULL descriptor check — prevents dereferencing a null pointer
	 * that would otherwise cause a kernel panic or memory corruption
	 * when accessing desc->tag.  This can happen if the shared stats
	 * page has not yet been initialised by the hypervisor. */
	if (!desc)
		return -EINVAL;

	/* Bounds check on the tag field — prevents out-of-bounds array
	 * access if the hypervisor writes an unrecognised tag value that
	 * would index past the end of the known-tag table.  Unknown tags
	 * are silently skipped by the caller. */
	if (desc->tag >= VMW_BALLOON_STAT_NR)
		return -EINVAL;

	/* Descriptor is structurally valid — safe to read its value. */
	return 0;
}

/**
 * vmw_balloon_collect_stats - Read validated stats descriptors from
 *                             the shared stats page.
 * @stats:  Output buffer for validated descriptors (must not be NULL).
 * @max:    Maximum number of descriptors to read (must be > 0).
 *
 * Iterates through the shared stats descriptor array.  Each descriptor
 * is validated by vmw_balloon_validate_stats_desc() before its data is
 * copied to the output buffer.  Unknown/invalid tags are silently
 * skipped so that a single corrupt descriptor does not stall the entire
 * statistics collection.  Iteration stops at the zero-tag end-of-list
 * sentinel or when @max descriptors have been collected.
 *
 * Returns the number of valid descriptors collected, or 0 if the
 * balloon is not present, parameters are invalid, or no valid
 * descriptors were found.
 */
static int
vmw_balloon_collect_stats(struct vmw_balloon_stats_desc *stats, int max)
{
	int i;
	int count = 0;

	/* State check: if the balloon device is not present or has not
	 * been initialised, there are no stats to collect.  Reading from
	 * the shared page in this state would return garbage data. */
	if (!vmw_balloon.present)
		return 0;

	/* Parameter validation: reject NULL output buffer or zero/non-positive
	 * max count, which would cause a buffer overrun or infinite loop. */
	if (!stats || max <= 0) {
		kprintf("[VMW-BALLOON] collect_stats: invalid arguments "
			"(stats=%p, max=%d)\n", (void *)stats, max);
		return 0;
	}

	for (i = 0; i < VMW_BALLOON_STATS_MAX && count < max; i++) {
		const struct vmw_balloon_stats_desc *desc =
			&vmw_balloon.stats_desc[i];

		/* End-of-list sentinel: tag == 0 and value == 0 means
		 * there are no more statistics descriptors to read. */
		if (desc->tag == 0 && desc->value == 0)
			break;

		/* Validate before reading — reject malformed descriptors.
		 * A single corrupt descriptor is skipped rather than aborting
		 * the entire collection pass, providing defence-in-depth
		 * against a buggy or malicious hypervisor. */
		if (vmw_balloon_validate_stats_desc(desc) < 0) {
			kprintf("[VMW-BALLOON] collect_stats: skipping "
				"invalid descriptor %d (tag=%u)\n",
				i, (unsigned int)desc->tag);
			continue;
		}

		/* Safe to read — both tag and value have been validated. */
		stats[count].tag   = desc->tag;
		stats[count].value = desc->value;
		count++;
	}

	return count;
}
