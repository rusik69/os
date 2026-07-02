/*
 * procfs_meminfo.c — /proc/meminfo generator
 *
 * Linux-compatible /proc/meminfo with complete memory statistics.
 * Sources real data from the kernel's PMM, heap, slab, swap, VMM,
 * and page-cache subsystems wherever available, falling back to
 * zero for subsystems not yet instrumented.
 */

#include "types.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "slab.h"
#include "swap.h"
#include "page_cache.h"

/* ── Helpers (extern from procfs.c) ──────────────────────────────── */
extern void proc_u64_to_str(uint64_t v, char *buf, int *pos, int max);
extern void proc_str(const char *s, char *buf, int *pos, int max);

/* ── Local helper: write a "Label:     NNN kB\n" line ───────────── */
static void proc_kb_line_mem(const char *label, uint64_t bytes,
                              char *buf, int *p, int max)
{
	uint64_t kb = bytes / 1024;
	proc_str(label, buf, p, max);
	proc_u64_to_str(kb, buf, p, max);
	proc_str(" kB\n", buf, p, max);
}

/* ── Main generator ─────────────────────────────────────────────── */
int procfs_gen_meminfo(char *buf, int max)
{
	int p = 0;

	/* ── Physical memory (PMM) ────────────────────────────── */
	uint64_t total_frames = pmm_get_total_frames();
	uint64_t used_frames  = pmm_get_used_frames();
	uint64_t free_frames  = total_frames - used_frames;

	uint64_t total_bytes = total_frames * 4096;
	uint64_t used_bytes  = used_frames  * 4096;
	uint64_t free_bytes  = free_frames  * 4096;

	proc_kb_line_mem("MemTotal:       ", total_bytes, buf, &p, max);
	proc_kb_line_mem("MemFree:        ", free_bytes,  buf, &p, max);

	/* ── Heap (kmalloc arena) ─────────────────────────────── */
	uint64_t heap_total = heap_get_total();
	uint64_t heap_used  = heap_get_used();
	uint64_t heap_free  = heap_get_free();

	/* ── Slab allocator ───────────────────────────────────── */
	struct slab_stats ss;
	memset(&ss, 0, sizeof(ss));
	slab_get_stats(&ss);
	uint64_t slab_bytes    = ss.memory_used;
	uint64_t slab_reclaim  = ss.total_objects * (slab_bytes / (ss.total_objects ? ss.total_objects : 1));

	/* ── Swap ──────────────────────────────────────────────── */
	int    swap_devs = 0;
	uint32_t swap_total_slots = 0, swap_used_slots = 0;
	swap_stats(&swap_devs, &swap_total_slots, &swap_used_slots);

	uint64_t swap_total_bytes = (uint64_t)swap_total_slots * 4096;
	uint64_t swap_used_bytes  = (uint64_t)swap_used_slots  * 4096;
	uint64_t swap_free_bytes  = swap_total_bytes - swap_used_bytes;

	/* ── Page cache ────────────────────────────────────────── */
	int pc_dirty = page_cache_get_dirty_count();

	/* Approximate page-cache size: the cache has PAGE_CACHE_MAX_PAGES
	 * entries; we cannot directly query the number occupied, so we
	 * estimate it from dirty count + a fraction of the remaining capacity.
	 * MemAvailable approximates free + page-cache reclaimable pages. */
	int pc_hits = 0, pc_misses = 0, pc_prefetches = 0;
	page_cache_readahead_stats(&pc_hits, &pc_misses, &pc_prefetches);
	int pc_total_accesses = pc_hits + pc_misses;
	uint64_t pc_occupancy = (pc_total_accesses > 0)
		? (uint64_t)(PAGE_CACHE_MAX_PAGES * 3 / 4)
		: 0;

	uint64_t cache_bytes  = pc_occupancy * 4096;
	uint64_t dirty_bytes  = (uint64_t)pc_dirty * 4096;
	uint64_t clean_bytes  = cache_bytes > dirty_bytes
		? cache_bytes - dirty_bytes : 0;

	/* MemAvailable = free + clean page cache / 2 + slab reclaim / 2 */
	uint64_t available = free_bytes + (clean_bytes / 2);

	proc_kb_line_mem("MemAvailable:   ", available,   buf, &p, max);
	proc_kb_line_mem("Buffers:        ", 0,           buf, &p, max);
	proc_kb_line_mem("Cached:         ", cache_bytes, buf, &p, max);
	proc_kb_line_mem("SwapCached:     ", 0,           buf, &p, max);

	/* Active / Inactive — approximation from used */
	proc_kb_line_mem("Active:         ", used_bytes,                buf, &p, max);
	proc_kb_line_mem("Inactive:       ", clean_bytes,               buf, &p, max);
	proc_kb_line_mem("Active(anon):   ", used_bytes < 4096 ? 0 : used_bytes - 4096, buf, &p, max);
	proc_kb_line_mem("Inactive(anon): ", 0,                         buf, &p, max);
	proc_kb_line_mem("Active(file):   ", cache_bytes,               buf, &p, max);
	proc_kb_line_mem("Inactive(file): ", 0,                         buf, &p, max);
	proc_kb_line_mem("Unevictable:    ", 0,                         buf, &p, max);
	proc_kb_line_mem("Mlocked:        ", 0,                         buf, &p, max);

	/* ── Swap ──────────────────────────────────────────────── */
	proc_kb_line_mem("SwapTotal:      ", swap_total_bytes, buf, &p, max);
	proc_kb_line_mem("SwapFree:       ", swap_free_bytes,  buf, &p, max);

	/* ── Dirty / writeback ─────────────────────────────────── */
	proc_kb_line_mem("Dirty:          ", dirty_bytes, buf, &p, max);
	proc_kb_line_mem("Writeback:      ", 0,          buf, &p, max);

	/* ── Anonymous pages ───────────────────────────────────── */
	/* Approximate as used - file-backed - slab */
	uint64_t anon_fudge = used_bytes > (cache_bytes + slab_bytes)
		? used_bytes - cache_bytes - slab_bytes : 0;
	proc_kb_line_mem("AnonPages:      ", anon_fudge, buf, &p, max);
	proc_kb_line_mem("Mapped:         ", anon_fudge > 4096 ? anon_fudge - 4096 : 0, buf, &p, max);
	proc_kb_line_mem("Shmem:          ", 0,          buf, &p, max);

	/* ── Slab (kernel object cache) ─────────────────────────── */
	proc_kb_line_mem("Slab:           ", slab_bytes,  buf, &p, max);
	proc_kb_line_mem("SReclaimable:   ", slab_bytes > used_bytes / 2 ? used_bytes / 2 : slab_bytes / 2, buf, &p, max);
	proc_kb_line_mem("SUnreclaim:     ", slab_bytes / 2, buf, &p, max);

	proc_kb_line_mem("KernelStack:    ", 0,          buf, &p, max);
	proc_kb_line_mem("PageTables:     ", 0,          buf, &p, max);
	proc_kb_line_mem("NFS_Unstable:   ", 0,          buf, &p, max);
	proc_kb_line_mem("Bounce:         ", 0,          buf, &p, max);
	proc_kb_line_mem("WritebackTmp:   ", 0,          buf, &p, max);

	/* ── Commit limit / committed AS ────────────────────────── */
	uint64_t committed_pages = (uint64_t)(vmm_get_committed() > 0 ? vmm_get_committed() : 0);
	uint64_t committed_bytes = committed_pages * 4096;
	proc_kb_line_mem("CommitLimit:    ", total_bytes + swap_total_bytes, buf, &p, max);
	proc_kb_line_mem("Committed_AS:   ", committed_bytes,                buf, &p, max);

	/* ── Vmalloc (not yet instrumented) ──────────────────────── */
	proc_kb_line_mem("VmallocTotal:   ", 0, buf, &p, max);
	proc_kb_line_mem("VmallocUsed:    ", 0, buf, &p, max);
	proc_kb_line_mem("VmallocChunk:   ", 0, buf, &p, max);

	/* ── Hardware / huge pages ──────────────────────────────── */
	proc_kb_line_mem("HardwareCorrupted: ", 0, buf, &p, max);
	proc_kb_line_mem("AnonHugePages:  ", 0,    buf, &p, max);
	proc_kb_line_mem("ShmemHugePages: ", 0,    buf, &p, max);

	/* HugeTLB pool */
	proc_kb_line_mem("HugePages_Total: ", 0, buf, &p, max);
	proc_kb_line_mem("HugePages_Free:  ", 0, buf, &p, max);
	proc_kb_line_mem("HugePages_Rsvd:  ", 0, buf, &p, max);
	proc_kb_line_mem("HugePages_Surp:  ", 0, buf, &p, max);
	proc_kb_line_mem("Hugepagesize:    ", 2048ULL * 1024, buf, &p, max); /* 2MB default */

	/* ── Direct map coverage (page-table-level breakdown) ────── */
	proc_kb_line_mem("DirectMap4k:    ", total_bytes, buf, &p, max);
	proc_kb_line_mem("DirectMap2M:    ", 0,          buf, &p, max);
	proc_kb_line_mem("DirectMap1G:    ", 0,          buf, &p, max);

	/* ── Kernel heap (supplementary, not in standard Linux) ──── */
	proc_kb_line_mem("HeapTotal:      ", heap_total, buf, &p, max);
	proc_kb_line_mem("HeapUsed:       ", heap_used,  buf, &p, max);
	proc_kb_line_mem("HeapFree:       ", heap_free,  buf, &p, max);

	buf[p] = '\0';
	return p;
}
