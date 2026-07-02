#ifndef NUMA_MEM_H
#define NUMA_MEM_H

#include "types.h"
#include "cpu_topology.h"

/*
 * numa_mem.h — NUMA-aware physical memory allocation
 *
 * Tracks per-node physical memory ranges and provides allocation
 * functions that prefer memory from a specific NUMA node.
 *
 * On systems with a single NUMA node (the common case), all memory
 * is on node 0 and allocations fall through to the standard PMM.
 */

/* ── Per-node memory range tracking ──────────────────────────────── */

/* Physical start address of each NUMA node's memory (0 = not set) */
extern uint64_t numa_node_memory_start[NUMA_MAX_NODES];

/* Physical end address (exclusive) of each NUMA node's memory */
extern uint64_t numa_node_memory_end[NUMA_MAX_NODES];

/* Initialise per-node memory ranges.  Called from numa_init(). */
void numa_mem_init(void);

/* ── NUMA-aware physical frame allocation ────────────────────────── */

/*
 * pmm_alloc_frame_on_node() - Allocate a physical frame from a
 * specific NUMA node's memory range.
 *
 * @node:  Target NUMA node (0 .. numa_node_count-1).
 *         If the node has no memory assigned, or allocation from
 *         that range fails, falls back to pmm_alloc_frame().
 *
 * Returns physical address of the frame, or 0 on failure.
 */
uint64_t pmm_alloc_frame_on_node(int node);

/*
 * alloc_pages_node() - Allocate 2^order physically contiguous pages
 * from a specific NUMA node.
 *
 * @node:     Target NUMA node.
 * @gfp_mask: GFP flags (GFP_KERNEL, GFP_ZERO, etc.).
 * @order:    Allocation size (2^order pages).
 *
 * Returns physical address of the first page, or 0 on failure.
 */
uint64_t alloc_pages_node(int node, int gfp_mask, int order);

/*
 * free_node_pages() - Free pages previously allocated via
 * alloc_pages_node().  Pages are freed to the standard PMM
 * regardless of which node they were allocated from.
 */
static inline void free_node_pages(uint64_t addr, int order)
{
	extern void free_pages(uint64_t addr, int order);
	free_pages(addr, order);
}

/* ── Accessor helpers ────────────────────────────────────────────── */

/*
 * numa_node_has_memory() - Returns 1 if the given NUMA node has a
 * non-zero physical memory range assigned.
 */
static inline int numa_node_has_memory(int node)
{
	if (node < 0 || node >= NUMA_MAX_NODES)
		return 0;
	return numa_node_memory_end[node] > numa_node_memory_start[node];
}

#endif /* NUMA_MEM_H */
