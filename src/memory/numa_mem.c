#define KERNEL_INTERNAL
#include "numa_mem.h"
#include "pmm.h"
#include "page_allocator_ext.h"
#include "printf.h"
#include "string.h"
#include "panic.h"

/*
 * numa_mem.c — Per-node physical memory management
 *
 * Tracks the physical memory range belonging to each NUMA node and
 * provides allocation functions that prefer the local node first.
 *
 * Default initialisation (no ACPI SRAT): distribute all available
 * physical memory equally across detected NUMA nodes.
 */

/* ── Per-node memory ranges ──────────────────────────────────────────── */

uint64_t numa_node_memory_start[NUMA_MAX_NODES] = {0};
uint64_t numa_node_memory_end[NUMA_MAX_NODES]   = {0};

/* Forward-declare PMM internals needed for fallback allocation */
extern uint64_t pmm_get_total_frames(void);
extern uint64_t pmm_get_used_frames(void);

/*
 * numa_mem_init() — Initialise per-node physical memory ranges
 *
 * Called once during kernel boot from numa_init().  Distributes all
 * physical memory evenly across the detected NUMA nodes.
 *
 * In a full implementation, this would parse the ACPI SRAT to get
 * accurate per-node memory affinity.  For now, on a single-node
 * system all memory is on node 0, and on multi-node systems we
 * partition equally.
 */
void __init numa_mem_init(void)
{
	uint64_t total_frames = pmm_get_total_frames();
	uint64_t frame_size   = (uint64_t)PAGE_SIZE;

	if (total_frames == 0) {
		/* Use a reasonable default (256 MB) */
		total_frames = (256ULL * 1024 * 1024) / PAGE_SIZE;
	}

	int nodes = numa_node_count;
	if (nodes < 1) nodes = 1;
	if (nodes > NUMA_MAX_NODES) nodes = NUMA_MAX_NODES;

	if (nodes == 1) {
		/* Single-node: all memory belongs to node 0 */
		numa_node_memory_start[0] = 0;
		numa_node_memory_end[0]   = total_frames * frame_size;
		kprintf("[NUMA-MEM] Node 0: 0x%llx - 0x%llx (%llu MB)\n",
		        (unsigned long long)numa_node_memory_start[0],
		        (unsigned long long)numa_node_memory_end[0],
		        (unsigned long long)((total_frames * frame_size) / (1024 * 1024)));
		return;
	}

	/* Multi-node: partition physical memory equally */
	uint64_t frames_per_node = total_frames / (uint64_t)nodes;
	uint64_t remainder       = total_frames % (uint64_t)nodes;

	uint64_t current_start = 0;
	for (int i = 0; i < nodes; i++) {
		uint64_t node_frames = frames_per_node;
		if (i == nodes - 1)
			node_frames += remainder; /* give leftovers to last node */

		numa_node_memory_start[i] = current_start * frame_size;
		numa_node_memory_end[i]   = (current_start + node_frames) * frame_size;

		kprintf("[NUMA-MEM] Node %d: 0x%llx - 0x%llx (%llu frames)\n",
		        i,
		        (unsigned long long)numa_node_memory_start[i],
		        (unsigned long long)numa_node_memory_end[i],
		        (unsigned long long)node_frames);

		current_start += node_frames;
	}
}

/*
 * pmm_alloc_frame_on_node() — Allocate a physical frame from a specific
 * NUMA node's memory region.
 *
 * Strategy: scan the bitmap starting from the node's memory range to
 * find a free frame within the node's boundaries.  If none is found,
 * fall back to the global pmm_alloc_frame().
 *
 * Returns physical address of the frame, or 0 on failure.
 */
uint64_t pmm_alloc_frame_on_node(int node)
{
	if (node < 0 || node >= NUMA_MAX_NODES)
		return 0;

	uint64_t start = numa_node_memory_start[node];
	uint64_t end   = numa_node_memory_end[node];

	if (end <= start) {
		/* Node has no memory — fall back to global allocator */
		return pmm_alloc_frame();
	}

	uint64_t start_frame = start / PAGE_SIZE;
	uint64_t end_frame   = end   / PAGE_SIZE;

	/* Scan within the node's range for a free frame */
	uint64_t count = 0;
	uint64_t free_frame = pmm_find_free_region(start_frame, &count);

	if (free_frame != ~0ULL && free_frame < end_frame) {
		/*
		 * Allocate this specific frame via pmm_alloc_frame
		 * (which does the actual bitmap management).  Since
		 * pmm_alloc_frame() doesn't do node-specific allocation,
		 * we just call it and trust it finds a free page.
		 * In a full implementation, the PMM bitmap would be
		 * split per-node; for now we rely on the hint-based
		 * allocation being within the right range.
		 */
		uint64_t frame = pmm_alloc_frame();
		if (frame != 0) {
			/* Verify it's within the requested node's range */
			uint64_t f = frame / PAGE_SIZE;
			if (f >= start_frame && f < end_frame)
				return frame;
			/* Wrong node — free and try again */
			pmm_free_frame(frame);
		}
	}

	/* Global fallback */
	return pmm_alloc_frame();
}

/*
 * alloc_pages_node() — Allocate contiguous pages with NUMA affinity
 *
 * @node:     Target NUMA node.
 * @gfp_mask: GFP flags (GFP_KERNEL, GFP_ZERO, etc.).
 * @order:    Allocation size (2^order pages).
 *
 * Returns physical address of the first page, or 0 on failure.
 *
 * For order > 0, falls back to the standard alloc_pages() since
 * contiguous multi-page allocations across NUMA boundaries are
 * handled by the PMM's contiguous frame allocator.
 */
uint64_t alloc_pages_node(int node, int gfp_mask, int order)
{
	if (order == 0) {
		uint64_t frame = pmm_alloc_frame_on_node(node);
		if (frame == 0)
			return 0;

		/* Apply GFP_ZERO if requested */
		if (gfp_mask & GFP_ZERO) {
			void *virt = PHYS_TO_VIRT(frame);
			memset(virt, 0, PAGE_SIZE);
		}

		return frame;
	}

	/* For multi-page allocations, use the standard allocator.
	 * In a full implementation, this would verify contiguity
	 * within a single node's range. */
	return alloc_pages(gfp_mask, order);
}

/* ── Module info ────────────────────────────────────────────────── */
#include "module.h"
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("NUMA-aware physical memory allocation");
MODULE_AUTHOR("OS Kernel Team");
