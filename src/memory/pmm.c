#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "oom.h"
#include "panic.h"
#include "scheduler.h"
#include "compaction.h"
#include "slab.h"
#include "io.h"
#include "spinlock.h"
#include "smp.h"
#include "export.h"
#include "pageblock.h"
#include "timer.h"    /* timer_get_ticks() for failure timestamps */
#include "psi.h"      /* psi_memstall_enter/leave for memory stall tracking */
#include "mglru.h"    /* Multi-Generational LRU page reclaim */

/* Multiboot1 info structure (relevant fields) */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
};

struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type; /* 1 = available */
} __attribute__((packed));

#define MAX_FRAMES (256 * 1024) /* up to 1GB with 4KB pages */
static uint8_t  frame_bitmap[MAX_FRAMES / 8];
static uint16_t frame_refcount[MAX_FRAMES]; /* COW reference counts */
static uint64_t total_frames = 0;
static uint64_t used_frames = 0;
static uint64_t pmm_hint = 0; /* last-known free frame; speeds up allocation */

/* Page poisoning: fill freed pages with 0xDC and allocated pages with 0xDEADBEEF */
int __read_mostly pmm_poison_enabled = 1;

/* Per-CPU page hot cache ────────────────────────────────────────────
 * Each CPU keeps a small pool of pre-allocated pages to avoid lock
 * contention on the global bitmap.  The hot cache is lock-free for the
 * owning CPU (only local IRQ save/restore is needed for reentrancy from
 * interrupt handlers on the same CPU).
 */
#define PMM_CPU_CACHE_SIZE 8

/* Memory zone count — guards zone-like migration type array accesses */
#define ZONE_MAX   MIGRATE_TYPES

struct pmm_cpu_cache {
    uint64_t frames[PMM_CPU_CACHE_SIZE]; /* cached physical page addresses */
    int      count;                       /* number of valid entries */
};

/* One cache slot per possible CPU */
static struct pmm_cpu_cache pmm_cpu_cache[SMP_MAX_CPUS];

/* Global spinlock protecting the bitmap and shared counters during
 * cache refill/drain operations.  The fast per-CPU path avoids this. */
static spinlock_t pmm_global_lock;

/* ── Allocation failure tracking ────────────────────────────────────────
 * Records recent page allocation failures with caller context for
 * post-mortem analysis and diagnostics.  Each record captures the
 * caller's return address, requested size/order, and timestamp.
 */
#define PMM_FAIL_HISTORY_MAX 32

struct pmm_fail_record {
    uint64_t caller_ip;       /* return address of failing allocator call */
    uint64_t requested;       /* number of pages requested */
    uint64_t free_at_fail;    /* free frames at time of failure */
    uint64_t timestamp_tick;  /* kernel tick when failure occurred */
};

/* Ring buffer of recent allocation failures (for diagnostics / post-mortem) */
static struct pmm_fail_record pmm_fail_history[PMM_FAIL_HISTORY_MAX];
static uint64_t pmm_fail_count_total = 0;   /* total failures since boot */
static uint64_t pmm_fail_history_idx = 0;   /* next slot in ring buffer */
static spinlock_t pmm_fail_lock;

/* Record an allocation failure in the ring buffer.  Safe to call from
 * any context (interrupts or not).  Uses trylock to avoid deadlock
 * if called from deep inside the OOM path where another lock may be held. */
static void pmm_record_fail(uint64_t caller_ip, uint64_t requested) {
    uint64_t irq_flags;
    /* Manual trylock: save flags, cli, then try to acquire */
    __asm__ volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli\n\t"
        : "=r"(irq_flags)
        :
        : "memory"
    );
    if (!spinlock_try_acquire(&pmm_fail_lock)) {
        /* Couldn't get lock — restore interrupts and skip recording */
        if (irq_flags & 0x200)
            __asm__ volatile("sti" : : : "memory");
        return;
    }

    struct pmm_fail_record *rec = &pmm_fail_history[pmm_fail_history_idx];
    rec->caller_ip      = caller_ip;
    rec->requested      = requested;
    rec->free_at_fail   = (total_frames > used_frames) ? (total_frames - used_frames) : 0;
    rec->timestamp_tick = timer_get_ticks();

    pmm_fail_history_idx = (pmm_fail_history_idx + 1) % PMM_FAIL_HISTORY_MAX;
    pmm_fail_count_total++;

    /* Release with matching IRQ restore */
    spinlock_irqsave_release(&pmm_fail_lock, irq_flags);
}

/* Dump the recorded allocation failure history for diagnostics. */
static void pmm_dump_fail_history(void) {
    uint64_t total = pmm_fail_count_total;
    uint64_t shown = (total < PMM_FAIL_HISTORY_MAX) ? total : PMM_FAIL_HISTORY_MAX;

    kprintf("[PMM] Allocation failures: %llu total (showing last %llu):\n",
            (unsigned long long)total, (unsigned long long)shown);

    if (shown == 0) {
        kprintf("[PMM]   (none)\n");
        return;
    }

    /* Walk the ring buffer oldest-first */
    uint64_t start_idx;
    if (total < PMM_FAIL_HISTORY_MAX)
        start_idx = 0;
    else
        start_idx = pmm_fail_history_idx;  /* points to oldest */

    for (uint64_t i = 0; i < shown; i++) {
        uint64_t idx = (start_idx + i) % PMM_FAIL_HISTORY_MAX;
        const struct pmm_fail_record *rec = &pmm_fail_history[idx];
        if (rec->caller_ip == 0 && rec->requested == 0)
            continue;  /* empty slot */

        /* Print caller address (symbol resolution would need ksymtab) */
        kprintf("[PMM]   [%llu] caller=0x%llx requested=%llu pages free=%llu ticks=%llu\n",
                (unsigned long long)i,
                (unsigned long long)rec->caller_ip,
                (unsigned long long)rec->requested,
                (unsigned long long)rec->free_at_fail,
                (unsigned long long)rec->timestamp_tick);
    }
}

/* ── Poison helpers ──────────────────────────────────────────────────── */
static void poison_fill(uint64_t phys_addr, uint32_t pattern) {
    if (!pmm_poison_enabled) return;
    if (phys_addr == 0) return;
    uint64_t *virt = (uint64_t *)PHYS_TO_VIRT(phys_addr);
    /* Fill 4KB page with 64-bit pattern */
    uint64_t pat64 = ((uint64_t)pattern << 32) | pattern;
    if (pattern == 0xDEADBEEF) {
        pat64 = 0xDEADBEEFDEADBEEFULL;
    }
    for (int i = 0; i < (int)(PAGE_SIZE / 8); i++)
        virt[i] = pat64;
}

static void bitmap_set(uint64_t frame) {
    if (frame >= MAX_FRAMES) return;
    frame_bitmap[frame / 8] |= (uint8_t)(1U << (frame % 8));
}

static void bitmap_clear(uint64_t frame) {
    if (frame >= MAX_FRAMES) return;
    frame_bitmap[frame / 8] &= (uint8_t)~(1U << (frame % 8));
}

static int bitmap_test(uint64_t frame) {
    if (frame >= MAX_FRAMES) return 1; /* out-of-range frames appear used */
    return frame_bitmap[frame / 8] & (1U << (frame % 8));
}

/* ── Internal bitmap allocator (caller must hold pmm_global_lock) ────── */

/* Allocate one frame from the bitmap; no lock acquired, no OOM recovery.
 * Returns physical address, or 0 on failure. */
static uint64_t bitmap_alloc_one_locked(void) {
    uint64_t i = pmm_hint;
    do {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
            frame_refcount[i] = 1;
            pmm_hint = i + 1;
            if (pmm_hint >= total_frames) pmm_hint = 0;

#ifdef CONFIG_DEBUG_PAGEALLOC
            /* ── Use-after-free detection ──────────────────────────────
             * Check if the page still contains the freed poison pattern
             * (0xDC).  If it does, it's a clean reuse.  If it doesn't,
             * someone may have written to it while it was freed, indicating
             * a use-after-free or buffer overflow that corrupted the page
             * while it was in the free pool. */
            if (pmm_poison_enabled) {
                uint64_t *virt = (uint64_t *)PHYS_TO_VIRT(i * PAGE_SIZE);
                int found_non_poison = 0;
                uint64_t poison64 = 0xDCDCDCDCDCDCDCULL;
                for (int w = 0; w < (int)(PAGE_SIZE / 8); w++) {
                    if (virt[w] != poison64) {
                        /* Skip first few bytes — they may have been
                         * overwritten by bitmap/free-list metadata.
                         * If the corruption is widespread, it's a UAF. */
                        if (w > 4) {
                            found_non_poison = 1;
                            break;
                        }
                    }
                }
                if (found_non_poison) {
                    kprintf("[PMM] WARNING: page 0x%llx (frame %llu) "
                            "does NOT contain poison pattern — "
                            "possible use-after-free!\n",
                            (unsigned long long)(i * PAGE_SIZE),
                            (unsigned long long)i);
                    /* Re-poison to be safe */
                    poison_fill(i * PAGE_SIZE, 0xDC);
                }
            }
#endif /* CONFIG_DEBUG_PAGEALLOC */

            return i * PAGE_SIZE;
        }
        i++;
        if (i >= total_frames) i = 0;
    } while (i != pmm_hint);
    return 0;
}

/* Free one frame back to the bitmap; caller must hold pmm_global_lock. */
static void bitmap_free_one_locked(uint64_t addr) {
    if (addr & (PAGE_SIZE - 1)) return;
    uint64_t frame = addr / PAGE_SIZE;
    if (frame >= MAX_FRAMES) return;
    if (!bitmap_test(frame)) return;
    if (frame_refcount[frame] > 1) return;

    poison_fill(addr, 0xDC);
    vm_pgfree++;
    bitmap_clear(frame);
    frame_refcount[frame] = 0;
    used_frames--;
}

/* ── Per-CPU hot-cache helpers ─────────────────────────────────────────
 * These must be called with local interrupts disabled to prevent
 * reentrancy from interrupt handlers on the same CPU.
 */

/* Refill the current CPU's cache from the global bitmap.
 * Takes pmm_global_lock, allocates up to PMM_CPU_CACHE_SIZE pages. */
static void pmm_cache_refill(void) {
    int cpu = smp_get_cpu_id();
    struct pmm_cpu_cache *cache = &pmm_cpu_cache[cpu];

    if (cache->count >= PMM_CPU_CACHE_SIZE)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&pmm_global_lock, &irq_flags);

    while (cache->count < PMM_CPU_CACHE_SIZE) {
        uint64_t frame = bitmap_alloc_one_locked();
        if (!frame) break;
        cache->frames[cache->count++] = frame;
    }

    spinlock_irqsave_release(&pmm_global_lock, irq_flags);
}

/* Drain the current CPU's cache back to the global bitmap.
 * Takes pmm_global_lock and frees all cached pages. */
static void pmm_cache_drain(void) {
    int cpu = smp_get_cpu_id();
    struct pmm_cpu_cache *cache = &pmm_cpu_cache[cpu];

    if (cache->count == 0)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&pmm_global_lock, &irq_flags);

    while (cache->count > 0) {
        cache->count--;
        bitmap_free_one_locked(cache->frames[cache->count]);
    }

    spinlock_irqsave_release(&pmm_global_lock, irq_flags);
}

extern char _kernel_end[];

/**
 * pmm_init - Initialize the Physical Memory Manager
 * @multiboot_info_phys: Physical address of the Multiboot info structure
 *
 * Initializes the physical memory manager by parsing the Multiboot memory
 * map to detect available RAM regions. Marks all frames as used initially,
 * then clears the bitmap for available memory regions reported by the
 * bootloader. Reserves kernel memory frames, advances the allocation hint
 * past the first 64 MB to avoid page-table corruption from huge-page self-
 * references, initializes spinlocks, pre-populates the boot CPU's hot cache,
 * and initializes pageblock migration type tracking.
 *
 * Context: Called once during kernel boot, before any memory allocations.
 *          No locking required as this runs on the BSP only.
 * Return: void.
 */
void __init pmm_init(uint64_t multiboot_info_phys) {
    /* Mark all frames as used initially */
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    used_frames = MAX_FRAMES;

    struct multiboot_info *mbi = (struct multiboot_info *)PHYS_TO_VIRT(multiboot_info_phys);

    /* Check if memory map is available (bit 6 of flags) */
    if (mbi->flags & (1U << 6)) {
        uint64_t mmap_addr = (uint64_t)PHYS_TO_VIRT(mbi->mmap_addr);
        uint64_t mmap_end = mmap_addr + mbi->mmap_length;

        while (mmap_addr < mmap_end) {
            struct multiboot_mmap_entry *entry = (struct multiboot_mmap_entry *)mmap_addr;

            if (entry->type == 1 && entry->addr + entry->len > 0x100000) {
                uint64_t start = entry->addr;
                uint64_t end = entry->addr + entry->len;

                if (start < 0x100000) start = 0x100000;

                uint64_t start_frame = (start + PAGE_SIZE - 1) / PAGE_SIZE;
                uint64_t end_frame = end / PAGE_SIZE;

                if (end_frame > MAX_FRAMES) end_frame = MAX_FRAMES;
                if (end_frame > total_frames) total_frames = end_frame;

                for (uint64_t f = start_frame; f < end_frame; f++) {
                    bitmap_clear(f);
                    used_frames--;
                }
            }
            mmap_addr += entry->size + 4; /* size field doesn't include itself */
        }
    } else {
        /* Fallback: use mem_upper (KB above 1MB) */
        uint64_t mem_bytes = (uint64_t)(mbi->mem_upper) * 1024 + 0x100000;
        uint64_t end_frame = mem_bytes / PAGE_SIZE;
        if (end_frame > MAX_FRAMES) end_frame = MAX_FRAMES;
        total_frames = end_frame;

        for (uint64_t f = 0x100000 / PAGE_SIZE; f < end_frame; f++) {
            bitmap_clear(f);
            used_frames--;
        }

        /* If mem_upper was 0 (QEMU -kernel multiboot fallback), assume 256MB */
        if (end_frame <= 256) {
            uint64_t assume_total = (256ULL * 1024 * 1024) / PAGE_SIZE;
            if (assume_total > MAX_FRAMES) assume_total = MAX_FRAMES;
            total_frames = assume_total;
            for (uint64_t f = 256; f < total_frames; f++) {
                bitmap_clear(f);
            }
        }
    }

    /* Reserve kernel frames — use physical address since _kernel_end has a high VMA */
    uint64_t kernel_end_phys = VIRT_TO_PHYS((uint64_t)(uintptr_t)_kernel_end);
    uint64_t kernel_end_frame = (kernel_end_phys + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t f = 0; f < kernel_end_frame && f < MAX_FRAMES; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
        }
    }

    /* Advance the allocation hint past the kernel binary + heap so
     * that page-table pages allocated later (via get_or_create_table)
     * never land within a 2 MB huge-page range that overlaps with
     * kernel data/heap.  If a page-table frame falls inside an active
     * huge page, subsequent writes through that huge page would corrupt
     * the page table (self-referential PTE corruption).  We skip the
     * first 64 MB to keep things simple. */
    {
        uint64_t safety_phys = 64ULL * 1024 * 1024; /* 64 MB */
        uint64_t safety_hint = safety_phys / PAGE_SIZE;
        if (safety_hint > pmm_hint)
            pmm_hint = safety_hint;
    }

    /* Recount used frames based on actual total */
    used_frames = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        if (bitmap_test(f)) used_frames++;
    }

    /* Initialize spinlocks for SMP-safe access */
    spinlock_init(&pmm_global_lock);
    spinlock_init(&pmm_fail_lock);

    /* Pre-populate the boot CPU's hot cache */
    pmm_cache_refill();

    /* Initialise pageblock migration type tracking */
    pageblock_init(total_frames);

    kprintf("[OK] Physical Memory Manager: %llu frames (%llu MB), %llu free\n",
            (unsigned long long)total_frames,
            (unsigned long long)((total_frames * 4ULL) / 1024ULL),
            (unsigned long long)(total_frames - used_frames));
}

void pmm_reserve_frames(uint64_t phys_start, uint64_t byte_size) {
    uint64_t start_frame = phys_start / PAGE_SIZE;
    uint64_t end_frame   = (phys_start + byte_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (end_frame > MAX_FRAMES) end_frame = MAX_FRAMES;

    /* Drain per-CPU caches so no cached frame overlaps with this range.
     * We iterate all possible CPUs (at boot, only BSP cache is populated). */
    for (int cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        struct pmm_cpu_cache *cache = &pmm_cpu_cache[cpu];
        if (cache->count == 0) continue;
        for (int j = 0; j < (int)cache->count; j++) {
            uint64_t f = cache->frames[j] / PAGE_SIZE;
            if (f >= start_frame && f < end_frame) {
                /* Remove this frame from the cache — pmm_reserve_frames
                 * will set the bit in the bitmap below, and the cached
                 * copy would let pmm_alloc_frame bypass the bitmap check. */
                cache->frames[j] = cache->frames[cache->count - 1];
                cache->count--;
                j--; /* re-check the swapped-in entry */
            }
        }
    }

    for (uint64_t f = start_frame; f < end_frame; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
        }
    }
}

void pmm_advance_hint(uint64_t phys_addr) {
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame + 1 > pmm_hint)
        pmm_hint = frame + 1;
}

/* ── Memory reclaim watermark ───────────────────────────────────────────
 * When free pages fall below watermark, kswapd-like reclaim is triggered.
 * Configurable via sysctl (see pmm_extras.c) or pmm_set_reclaim_watermark().
 */
static uint64_t pmm_reclaim_watermark = 64; /* default: 64 pages = 256 KB */

uint64_t pmm_get_reclaim_watermark(void) {
    return pmm_reclaim_watermark;
}

void pmm_set_reclaim_watermark(uint64_t pages) {
    pmm_reclaim_watermark = pages;
    kprintf("[pmm] reclaim watermark set to %llu pages\n", (unsigned long long)pages);
}

int pmm_below_watermark(void) {
    uint64_t free_pages = (total_frames > used_frames) ? (total_frames - used_frames) : 0;
    return free_pages < pmm_reclaim_watermark;
}

/* ── Memory statistics dumping ──────────────────────────────────────────── */

/* Scan the bitmap to find the largest contiguous free block (in frames) */
uint64_t pmm_largest_free_block(void) {
    uint64_t max_run = 0, cur_run = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        if (!bitmap_test(f)) {
            cur_run++;
        } else {
            if (cur_run > max_run) max_run = cur_run;
            cur_run = 0;
        }
    }
    if (cur_run > max_run) max_run = cur_run;
    return max_run;
}

/* Count distinct free page runs (higher = more fragmented) */
uint64_t pmm_free_block_count(void) {
    uint64_t runs = 0;
    int in_run = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        if (!bitmap_test(f)) {
            if (!in_run) { runs++; in_run = 1; }
        } else {
            in_run = 0;
        }
    }
    return runs;
}

/* ── Free-region scanner (for memory compaction) ──────────────────────── */

/* Scan the bitmap for the first free region starting at or after start_frame.
 * Returns the starting frame number, or ~0ULL if no free region remains.
 * On success, *out_count is set to the number of contiguous free frames. */
uint64_t pmm_find_free_region(uint64_t start_frame, uint64_t *out_count)
{
    if (out_count)
        *out_count = 0;
    if (start_frame >= total_frames)
        return ~0ULL;

    uint64_t f;
    uint64_t run_start = 0;
    int in_run = 0;

    for (f = start_frame; f < total_frames; f++) {
        if (!bitmap_test(f)) {
            if (!in_run) {
                run_start = f;
                in_run = 1;
            }
        } else {
            if (in_run) {
                if (out_count)
                    *out_count = f - run_start;
                return run_start;
            }
        }
    }

    /* If we finish in a run, report it */
    if (in_run) {
        if (out_count)
            *out_count = f - run_start;
        return run_start;
    }

    return ~0ULL;
}

/* Print detailed physical memory state: usage, largest free block, fragmentation */
void pmm_dump_stats(void) {
    uint64_t total = total_frames;
    uint64_t used  = used_frames;
    uint64_t free  = (total > used) ? (total - used) : 0;
    uint64_t free_pct = (total > 0) ? (free * 100ULL) / total : 0;

    kprintf("[PMM] frames: total=%llu (%llu MB), used=%llu, free=%llu (%llu%%)\n",
            (unsigned long long)total, (unsigned long long)((total * 4ULL) / 1024ULL),
            (unsigned long long)used, (unsigned long long)free,
            (unsigned long long)free_pct);

    uint64_t max_run   = pmm_largest_free_block();
    uint64_t free_runs = pmm_free_block_count();

    uint64_t frag_pct = (free > 0) ? ((free_runs * 100ULL) / free) : 0;
    if (frag_pct > 100) frag_pct = 100;

    kprintf("[PMM] largest free block: %llu frames (%llu KB), free runs: %llu, frag: %llu%%\n",
            (unsigned long long)max_run, (unsigned long long)(max_run * 4ULL),
            (unsigned long long)free_runs, (unsigned long long)frag_pct);

    /* Append OOM subsystem statistics */
    extern uint64_t oom_kill_count;
    kprintf("[PMM] OOM kills: %llu  |  pgalloc=%llu pgfree=%llu pgfault=%llu\n",
            (unsigned long long)oom_kill_count,
            (unsigned long long)vm_pgalloc,
            (unsigned long long)vm_pgfree,
            (unsigned long long)vm_pgfault);

    /* Allocation failure history */
    pmm_dump_fail_history();

    /* Per-CPU hot cache occupancy */
    int total_cached = 0;
    for (int c = 0; c < smp_get_cpu_count(); c++)
        total_cached += pmm_cpu_cache[c].count;
    kprintf("[PMM] per-CPU caches: %d frames cached across %d CPUs\n",
            total_cached, smp_get_cpu_count());

    /* Dump pageblock migration type distribution */
    pageblock_dump_stats();
}

/* ── Cache pop with IRQ save/restore ────────────────────────────────────
 * Attempt to pop one frame from the current CPU's hot cache.
 * If successful, writes the phys address to *addr_out and returns 1.
 * If the cache is empty, returns 0.
 * In both cases, *irq_save_out holds the saved RFLAGS (caller must restore).
 * Saves/restores asm to avoid duplication across the allocator paths. */
static inline int pmm_cache_pop_irqsafe(uint64_t *addr_out, uint64_t *irq_save_out) {
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(*irq_save_out) : : "memory");

    int cpu = smp_get_cpu_id();
    struct pmm_cpu_cache *cache = &pmm_cpu_cache[cpu];

    if (cache->count > 0) {
        cache->count--;
        *addr_out = cache->frames[cache->count];
        return 1;  /* caller must restore IRQs via pmm_irq_restore() */
    }
    return 0;
}

/* Restore interrupt state from a prior pmm_cache_pop_irqsave(). */
static inline void pmm_irq_restore(uint64_t irq_save) {
    if (irq_save & 0x200)
        __asm__ volatile("sti" : : : "memory");
}

/* ── Proactive low-memory reclaim ───────────────────────────────────────
 * Check if free memory is below the critical watermark and try to reclaim
 * some pages before we hit the full OOM path.  Returns 1 if reclaim freed
 * any pages (caller should retry allocation), 0 otherwise.
 *
 * The watermark is a configurable threshold (default: 64 pages = 256 KB).
 * It can be adjusted via sysctl or the pmm_set_reclaim_watermark() API
 * declared in pmm.h / pmm_extras.c.
 */
static int pmm_proactive_reclaim(uint64_t needed_pages) {
    uint64_t total = total_frames;
    uint64_t used  = used_frames;
    uint64_t free_pages = (total > used) ? (total - used) : 0;

    /* Get the current reclaim watermark */
    uint64_t watermark = pmm_reclaim_watermark;

    /* If free pages are still above watermark, no reclaim needed yet */
    if (free_pages >= watermark + needed_pages)
        return 0;

    kprintf("[PMM] Low memory: %llu free pages (watermark=%llu), attempting proactive reclaim...\n",
            (unsigned long long)free_pages, (unsigned long long)watermark);

    uint64_t before_free = free_pages;

    /* Stage 1: Shrink dentry cache — no-IPI, cheap */
    extern int dcache_shrink(uint64_t target);
    dcache_shrink(256);

    /* Stage 2: Reap empty slab pages */
    extern void kmem_cache_reap(void);
    kmem_cache_reap();

    /* Stage 3: MGLRU page reclaim — evict pages from oldest generation */
    {
        int reclaimed = mglru_reclaim_pages(64, 0);
        if (reclaimed > 0) {
            kprintf("[PMM] MGLRU reclaim freed %d pages\n", reclaimed);
        }
    }

    /* Check if reclaim made progress */
    uint64_t after_used = used_frames;
    uint64_t after_free = (total > after_used) ? (total - after_used) : 0;

    if (after_free > before_free) {
        kprintf("[PMM] Proactive reclaim freed %llu pages\n",
                (unsigned long long)(after_free - before_free));
        return 1;
    }

    return 0;
}

/* ── OOM recovery helper ───────────────────────────────────────────────
 * Attempts multi-level recovery when the page allocator cannot satisfy
 * a request.  Runs progressively stronger recovery steps, retrying the
 * per-CPU cache after each step.
 *
 * Returns the physical address of a successfully allocated page on success,
 * or 0 if all recovery levels failed.
 *
 * 'needed_pages' is passed to oom_kill() so the OOM killer knows how
 * much memory needs to be freed.
 *
 * The caller_ip is recorded in the allocation failure history on panic. */
static uint64_t pmm_oom_recover(uint64_t needed_pages, uint64_t caller_ip) {
    /*
     * Recovery levels:
     *   Level 1: Dentry cache shrink + slab reaping + OOM kill + yield
     *   Level 2: Compaction + OOM kill + yield
     *   Level 3: panic() with full diagnostics
     */
    static const char *const level_names[] = {
        "slab reaping + OOM",
        "compaction + OOM"
    };

    for (int level = 0; level < 2; level++) {
        kprintf("[PMM] OOM recovery level %d/%d: %s...\n",
                level + 1, 2, level_names[level]);

        if (level == 0) {
            /* Level 1: Try quick reclaims first — no OOM kill yet */
            extern int dcache_shrink(uint64_t target);
            dcache_shrink(512);

            extern void kmem_cache_reap(void);
            kmem_cache_reap();

            /* If still tight, call OOM killer */
            oom_kill(needed_pages > 0 ? needed_pages : 1);
        } else {
            /* Level 2: Run compaction to defragment, then more aggressive OOM */
            compaction_run();
            oom_kill(needed_pages > 0 ? needed_pages : 1);
        }

        /* Yield to let the killed process exit and free memory */
        scheduler_yield();

        /* Refill the per-CPU cache with newly freed pages */
        pmm_cache_refill();

        /* Try the cache */
        uint64_t addr = 0;
        uint64_t irq_save;
        if (pmm_cache_pop_irqsafe(&addr, &irq_save)) {
            /* Success — frame allocated after recovery */
            pmm_irq_restore(irq_save);
            poison_fill(addr, 0xDEADBEEF);
            vm_pgalloc++;
            return addr;  /* return the physical address directly */
        }
        pmm_irq_restore(irq_save);
    }

    /* ── All recovery levels exhausted — return error ── */
    pmm_record_fail(caller_ip, needed_pages);
    pmm_dump_stats();
    kprintf("[PMM] Out of memory — OOM killer and compaction failed to reclaim any frames\n");
    return 0;
}

/* ── Page reclaim entry point ────────────────────────────────────────────
 * Called when the kernel needs to free pages to avoid OOM.
 * If MGLRU is enabled, uses multi-generational LRU reclaim.
 * Otherwise, returns 0 (no fallback yet).
 *
 * @nr_pages   Number of pages to try to reclaim
 * @gfp_mask   Allocation context flags (reserved for future use)
 *
 * Returns the number of pages actually freed, or negative on error.
 */
int page_reclaim(int nr_pages, unsigned int gfp_mask)
{
    if (nr_pages <= 0)
        return 0;

    return mglru_reclaim_pages(nr_pages, gfp_mask);
}

/* ── Page allocator ─────────────────────────────────────────────────────── */

/**
 * pmm_alloc_frame - Allocate a single physical memory frame (page)
 *
 * Allocates a 4 KB physical frame. Fast path: pops from the per-CPU hot
 * cache with interrupts disabled. If the cache is empty, triggers proactive
 * reclaim and refills from the global bitmap. On OOM, runs full OOM recovery
 * and retries. Poison-fills the page with 0xDEADBEEF if page poisoning is
 * enabled. Updates VM statistics and MGLRU tracking.
 *
 * Context: Any context (SMP-safe via per-CPU cache). May sleep during OOM
 *          recovery. Must not be called from interrupt context if OOM path
 *          might block.
 * Return: Physical address of the allocated frame, or 0 on failure.
 */
uint64_t pmm_alloc_frame(void) {
    uint64_t addr = 0;
    uint64_t irq_save;

    /* ── Fast path: pop from per-CPU hot cache ── */
    if (pmm_cache_pop_irqsafe(&addr, &irq_save)) {
        pmm_irq_restore(irq_save);
        poison_fill(addr, 0xDEADBEEF);
        vm_pgalloc++;
        mglru_add_page(addr);
        return addr;
    }
    pmm_irq_restore(irq_save);

    /* ── Proactive reclaim check ──
     * Before going to the global bitmap, check if we're near the
     * reclaim watermark and try to free some pages preemptively.
     * This reduces the probability of hitting the full OOM path. */
    pmm_proactive_reclaim(1);

    /* ── Slow path: refill cache from global bitmap ── */
    pmm_cache_refill();

    if (pmm_cache_pop_irqsafe(&addr, &irq_save)) {
        pmm_irq_restore(irq_save);
        poison_fill(addr, 0xDEADBEEF);
        vm_pgalloc++;
        mglru_add_page(addr);
        return addr;
    }
    pmm_irq_restore(irq_save);

    /* ── Out of memory — run full OOM recovery ── */
    uint64_t caller_ip = (uint64_t)__builtin_return_address(0);
    pmm_record_fail(caller_ip, 1);

    psi_memstall_enter();
    uint64_t recovered = pmm_oom_recover(1, caller_ip);
    psi_memstall_leave();

    if (recovered)
        return recovered;

    return 0;
}

/* Allocate count contiguous frames. Returns first frame physical addr, or 0 on failure. */
uint64_t *pmm_alloc_frames(size_t count) {
    if (count == 0) return NULL;

    /* Single-frame allocations go through the fast per-CPU cache path */
    if (count == 1)
        return (uint64_t *)pmm_alloc_frame();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&pmm_global_lock, &irq_flags);

    /* ── Proactive reclaim check ──
     * Before scanning the bitmap for contiguous space, check if we're
     * near the watermark and try early reclaim.  This is especially
     * important for multi-page allocations which are harder to satisfy. */
    uint64_t total = total_frames;
    uint64_t used  = used_frames;
    uint64_t free_pages = (total > used) ? (total - used) : 0;
    uint64_t watermark = pmm_reclaim_watermark;

    if (free_pages < watermark + count) {
        spinlock_irqsave_release(&pmm_global_lock, irq_flags);
        pmm_proactive_reclaim(count);
        spinlock_irqsave_acquire(&pmm_global_lock, &irq_flags);
    }

    /* Scan for 'count' contiguous free frames */
    uint64_t start = pmm_hint;
    uint64_t found = 0;

    uint64_t i = pmm_hint;
    do {
        if (!bitmap_test(i)) {
            if (found == 0) start = i;
            found++;
            if (found == count) {
                /* Allocate all frames */
                for (uint64_t j = start; j < start + count; j++) {
                    bitmap_set(j);
                    used_frames++;
                    frame_refcount[j] = 1;
                    poison_fill(j * PAGE_SIZE, 0xDEADBEEF);
                }
                pmm_hint = start + count;
                if (pmm_hint >= total_frames) pmm_hint = 0;
                spinlock_irqsave_release(&pmm_global_lock, irq_flags);
                /* Track each allocated frame in MGLRU */
                for (uint64_t j = start; j < start + count; j++)
                    mglru_add_page(j * PAGE_SIZE);
                return (uint64_t *)(start * PAGE_SIZE);
            }
        } else {
            found = 0;
        }
        i++;
        if (i >= total_frames) i = 0;
    } while (i != pmm_hint);

    spinlock_irqsave_release(&pmm_global_lock, irq_flags);

    /* ── Out of memory ── */
    uint64_t caller_ip = (uint64_t)__builtin_return_address(0);
    pmm_record_fail(caller_ip, count);

    psi_memstall_enter();

    /* Run OOM recovery and retry */
    for (int level = 0; level < 2; level++) {
        kprintf("[PMM] OOM recovery level %d for %llu contiguous frames: ",
                level + 1, (unsigned long long)count);

        if (level == 0) {
            kprintf("slab reaping + OOM...\n");
            kmem_cache_reap();
            oom_kill(count);
        } else {
            kprintf("compaction + OOM...\n");
            compaction_run();
            oom_kill(count);
        }

        scheduler_yield();

        /* Retry the allocation */
        spinlock_irqsave_acquire(&pmm_global_lock, &irq_flags);
        start = pmm_hint;
        found = 0;
        i = pmm_hint;
        do {
            if (!bitmap_test(i)) {
                if (found == 0) start = i;
                found++;
                if (found == count) {
                    for (uint64_t j = start; j < start + count; j++) {
                        bitmap_set(j);
                        used_frames++;
                        frame_refcount[j] = 1;
                        poison_fill(j * PAGE_SIZE, 0xDEADBEEF);
                    }
                    pmm_hint = start + count;
                    if (pmm_hint >= total_frames) pmm_hint = 0;
                    spinlock_irqsave_release(&pmm_global_lock, irq_flags);
                    psi_memstall_leave();
                    /* Track each allocated frame in MGLRU */
                    for (uint64_t j = start; j < start + count; j++)
                        mglru_add_page(j * PAGE_SIZE);
                    return (uint64_t *)(start * PAGE_SIZE);
                }
            } else {
                found = 0;
            }
            i++;
            if (i >= total_frames) i = 0;
        } while (i != pmm_hint);
        spinlock_irqsave_release(&pmm_global_lock, irq_flags);
    }

    psi_memstall_leave();

    /* ── Final: return error — callers must handle ENOMEM ── */
    pmm_dump_stats();
    kprintf("[PMM] Out of memory — cannot allocate %llu contiguous frames\n",
          (unsigned long long)count);
    return NULL;
}

/**
 * pmm_free_frame - Free a single physical memory frame (page)
 * @addr: Physical address of the frame to free
 *
 * Frees a 4 KB physical frame back to the allocator. Fast path: pushes to
 * the per-CPU hot cache if space is available. If the cache is full, drains
 * the cache to the global bitmap first, then adds the frame. Falls back to
 * direct bitmap free if the cache remains full. Removes the page from MGLRU
 * tracking before recycling. Updates refcounts and page poisoning.
 *
 * Context: Any context (SMP-safe via per-CPU cache with interrupt masking).
 *          Must not be called with pmm_global_lock already held.
 * Return: void.
 */
void pmm_free_frame(uint64_t addr) {
    if (addr & (PAGE_SIZE - 1)) return;

    /* Remove from MGLRU tracking before recycling */
    mglru_remove_page(addr);

    int cpu = smp_get_cpu_id();
    struct pmm_cpu_cache *cache = &pmm_cpu_cache[cpu];

    /* ── Fast path: push to per-CPU hot cache if room ── */
    uint64_t irq_save;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(irq_save) : : "memory");

    if (cache->count < PMM_CPU_CACHE_SIZE) {
        cache->frames[cache->count++] = addr;
        if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");
        return;
    }

    if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");

    /* ── Slow path: drain cache to global, then free ── */
    pmm_cache_drain();

    /* Now add the new page (should succeed since we just drained) */
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(irq_save) : : "memory");
    if (cache->count < PMM_CPU_CACHE_SIZE) {
        cache->frames[cache->count++] = addr;
        if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");
        return;
    }
    if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");

    /* Fallback: direct free to global if cache still full */
    uint64_t frame = addr / PAGE_SIZE;
    if (frame >= MAX_FRAMES) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&pmm_global_lock, &irq_flags);
    bitmap_free_one_locked(addr);
    spinlock_irqsave_release(&pmm_global_lock, irq_flags);
}

/* Free 'count' contiguous physical frames starting at 'phys'.
 * Bypasses the per-CPU hot cache for efficiency with bulk operations. */
void pmm_free_frames_contiguous(uint64_t phys, size_t count) {
    if (count == 0 || phys == 0) return;
    if (phys & (PAGE_SIZE - 1)) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&pmm_global_lock, &irq_flags);

    for (size_t i = 0; i < count; i++) {
        uint64_t addr = phys + i * PAGE_SIZE;
        uint64_t frame = addr / PAGE_SIZE;
        if (frame >= MAX_FRAMES) break;
        if (!bitmap_test(frame)) continue;
        if (frame_refcount[frame] > 1) continue;

        mglru_remove_page(addr);
        poison_fill(addr, 0xDC);
        vm_pgfree++;
        bitmap_clear(frame);
        frame_refcount[frame] = 0;
        used_frames--;
    }

    spinlock_irqsave_release(&pmm_global_lock, irq_flags);
}

void pmm_ref_frame(uint64_t phys) {
    uint64_t frame = phys / PAGE_SIZE;
    if (frame < MAX_FRAMES && frame_refcount[frame] < 65535)
        frame_refcount[frame]++;
}

int pmm_unref_frame(uint64_t phys) {
    uint64_t frame = phys / PAGE_SIZE;
    if (frame >= MAX_FRAMES) return 0;
    if (frame_refcount[frame] == 0) return 0;
    frame_refcount[frame]--;
    if (frame_refcount[frame] == 0) {
        bitmap_clear(frame);
        used_frames--;
    }
    return (int)frame_refcount[frame];
}

int pmm_refcount(uint64_t phys) {
    uint64_t frame = phys / PAGE_SIZE;
    if (frame >= MAX_FRAMES) return 0;
    return (int)frame_refcount[frame];
}

uint64_t pmm_get_total_frames(void) { return total_frames; }
uint64_t pmm_get_used_frames(void)  { return used_frames; }

void pmm_set_poison(int enable) {
    pmm_poison_enabled = enable;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Pageblock Migration Types — Anti-Fragmentation (Item 121)
 *
 *  Physical memory is divided into 2 MB pageblocks (512 × 4 KB frames).
 *  Each pageblock is tagged with a migration type that segregates movable
 *  from unmovable allocations, preventing long-term fragmentation.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Per-pageblock migration type, one byte per 2 MB block.
 * Initialised in pageblock_init(). */
static uint8_t pageblock_types[PAGEBLOCK_MAX];

/* Fallback ordering — when the preferred migration type is exhausted,
 * try these alternatives in the order listed.  MIGRATE_CMA is not a
 * fallback target for non-CMA callers (it is reservation-only).
 * MIGRATE_ISOLATE is temporary and never used for regular allocation. */
const enum migratetype fallbacks[MIGRATE_TYPES][MIGRATE_TYPES] = {
    /* MIGRATE_UNMOVABLE   → RECLAIMABLE → MOVABLE → CMA */
    { MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE,     MIGRATE_CMA,     MIGRATE_ISOLATE },
    /* MIGRATE_MOVABLE     → RECLAIMABLE → UNMOVABLE → CMA */
    { MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE,   MIGRATE_CMA,     MIGRATE_ISOLATE },
    /* MIGRATE_RECLAIMABLE → UNMOVABLE → MOVABLE → CMA */
    { MIGRATE_UNMOVABLE,   MIGRATE_MOVABLE,     MIGRATE_CMA,     MIGRATE_ISOLATE },
    /* MIGRATE_CMA         → MOVABLE → RECLAIMABLE → UNMOVABLE */
    { MIGRATE_MOVABLE,     MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE, MIGRATE_ISOLATE },
    /* MIGRATE_ISOLATE     → no fallback (should never be allocated from) */
    { MIGRATE_ISOLATE,     MIGRATE_ISOLATE,     MIGRATE_ISOLATE, MIGRATE_ISOLATE },
};

void pageblock_init(uint64_t frames)
{
    uint64_t num_blocks = (frames + PAGEBLOCK_NR_PAGES - 1) >> PAGEBLOCK_ORDER;
    if (num_blocks > PAGEBLOCK_MAX)
        num_blocks = PAGEBLOCK_MAX;

    /* All pageblocks start as MOVABLE by default.  The boot code and
     * kernel subsystems should mark special regions as UNMOVABLE or
     * RECLAIMABLE via pageblock_set_migratetype(). */
    memset(pageblock_types, MIGRATE_MOVABLE, (size_t)num_blocks);

    kprintf("[PMM] pageblocks: %llu blocks of %lu KB each (default MOVABLE)\n",
            (unsigned long long)num_blocks,
            (unsigned long)(PAGEBLOCK_SIZE / 1024));
}

enum migratetype pageblock_get_migratetype(uint64_t frame)
{
    uint64_t block = pageblock_of_frame(frame);
    if (block >= PAGEBLOCK_MAX)
        return MIGRATE_MOVABLE;
    return (enum migratetype)pageblock_types[block];
}

void pageblock_set_migratetype(uint64_t frame, enum migratetype mt)
{
    uint64_t block = pageblock_of_frame(frame);
    if (block >= PAGEBLOCK_MAX)
        return;
    if (mt >= MIGRATE_TYPES)
        return;
    pageblock_types[block] = (uint8_t)mt;
}

/* Internal: scan a pageblock for a free frame.  Returns phys addr or 0. */
static uint64_t pageblock_scan_block(uint64_t block_idx)
{
    uint64_t base_frame = block_idx << PAGEBLOCK_ORDER;
    uint64_t end_frame = base_frame + PAGEBLOCK_NR_PAGES;
    if (end_frame > total_frames)
        end_frame = total_frames;

    for (uint64_t f = base_frame; f < end_frame; f++) {
        if (!bitmap_test(f)) {
            bitmap_set(f);
            used_frames++;
            frame_refcount[f] = 1;
            pmm_hint = f + 1;
            if (pmm_hint >= total_frames)
                pmm_hint = 0;
            return f * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pageblock_alloc_from_type(enum migratetype mt, uint64_t start_hint)
{
    if (mt >= MIGRATE_TYPES || mt == MIGRATE_ISOLATE)
        return 0;

    uint64_t num_blocks = (total_frames + PAGEBLOCK_NR_PAGES - 1) >> PAGEBLOCK_ORDER;
    if (num_blocks > PAGEBLOCK_MAX)
        num_blocks = PAGEBLOCK_MAX;
    if (num_blocks == 0)
        return 0;

    uint64_t start_block = pageblock_of_frame(start_hint / PAGE_SIZE);
    if (start_block >= num_blocks)
        start_block = 0;

    /* Search pageblocks of matching type */
    for (uint64_t bi = 0; bi < num_blocks; bi++) {
        uint64_t b = (start_block + bi) % num_blocks;
        if ((enum migratetype)pageblock_types[b] != mt)
            continue;
        uint64_t addr = pageblock_scan_block(b);
        if (addr != 0)
            return addr;
    }

    return 0;
}

void pageblock_dump_stats(void)
{
    uint64_t num_blocks = (total_frames + PAGEBLOCK_NR_PAGES - 1) >> PAGEBLOCK_ORDER;
    if (num_blocks > PAGEBLOCK_MAX)
        num_blocks = PAGEBLOCK_MAX;

    unsigned counts[MIGRATE_TYPES] = {0};
    for (uint64_t i = 0; i < num_blocks; i++) {
        enum migratetype mt = (enum migratetype)pageblock_types[i];
        if (mt < MIGRATE_TYPES)
            counts[mt]++;
    }

    kprintf("[PMM] pageblocks: UNMOVABLE=%u MOVABLE=%u RECLAIMABLE=%u CMA=%u\n",
            counts[MIGRATE_UNMOVABLE],
            counts[MIGRATE_MOVABLE],
            counts[MIGRATE_RECLAIMABLE],
            counts[MIGRATE_CMA]);
}

/* ── Migration-type aware allocation ──────────────────────────────────────
 *
 * pmm_alloc_frame_migrate() allocates a single frame, preferring pageblocks
 * of the given migration type.  If none found in the preferred type, falls
 * back through other types per the fallbacks[] table.
 *
 * The original pmm_alloc_frame() remains unchanged — it uses MIGRATE_MOVABLE
 * as the default type, preserving backward compatibility for all existing
 * callers (anonymous pages, page cache, general-purpose allocations).
 *
 * Callers that know their allocation will be long-lived and immovable
 * (kernel page tables, VMM structures, boot allocator backing) should use
 * pmm_alloc_frame_migrate(MIGRATE_UNMOVABLE) to help prevent fragmentation.
 */

uint64_t pmm_alloc_frame_migrate(enum migratetype mt)
{
    if (mt >= MIGRATE_TYPES || mt == MIGRATE_ISOLATE)
        mt = MIGRATE_MOVABLE;

    /* Try the preferred type first, then fallbacks */
    for (int attempt = 0; attempt < MIGRATE_TYPES; attempt++) {
        enum migratetype try_mt;
        if (attempt == 0)
            try_mt = mt;
        else
            try_mt = fallbacks[mt][attempt - 1];

        if (try_mt >= MIGRATE_TYPES || try_mt == MIGRATE_ISOLATE)
            continue;

        uint64_t addr = pageblock_alloc_from_type(try_mt, pmm_hint * PAGE_SIZE);
        if (addr != 0) {
            poison_fill(addr, 0xDEADBEEF);
            vm_pgalloc++;
            mglru_add_page(addr);
            return addr;
        }
    }

    /* All types exhausted — fall through to OOM recovery (same as
     * pmm_alloc_frame's slow path).  Use the per-CPU cache path which
     * will trigger OOM/compaction. */
    return pmm_alloc_frame();
}

/* ── Exported symbols for module loading ──────────────────────────── */
EXPORT_SYMBOL(pmm_alloc_frame);
EXPORT_SYMBOL(pmm_free_frame);
EXPORT_SYMBOL(pmm_ref_frame);

/* ── pmm_defrag ───────────────────────────────────────── */
static int pmm_defrag(void)
{
    kprintf("[pmm] pmm_defrag: defragmenting physical memory\n");
    /* Compact physical memory, merge buddies.
     * Delegate to the compaction subsystem. */
    uint64_t moved = compaction_run();
    kprintf("[pmm] pmm_defrag: %llu pages moved\n", (unsigned long long)moved);
    return (int)moved;
}

/* ── pmm_reclaim ───────────────────────────────────────── */
static int pmm_reclaim(int nr_pages)
{
    if (nr_pages <= 0)
        return 0;

    kprintf("[pmm] pmm_reclaim: trying to reclaim %d pages\n", nr_pages);

    /* Try MGLRU reclaim first */
    int reclaimed = mglru_reclaim_pages(nr_pages, 0);
    if (reclaimed >= nr_pages) {
        kprintf("[pmm] pmm_reclaim: MGLRU reclaimed %d pages\n", reclaimed);
        return reclaimed;
    }

    /* Shrink slab caches */
    extern void kmem_cache_reap(void);
    kmem_cache_reap();

    /* Try compaction to free contiguous blocks */
    compaction_run();

    uint64_t total = pmm_get_total_frames();
    uint64_t used = pmm_get_used_frames();
    uint64_t free_pages = (total > used) ? (total - used) : 0;

    kprintf("[pmm] pmm_reclaim: freed pages, now %llu free\n",
            (unsigned long long)free_pages);
    return (int)(free_pages < (uint64_t)nr_pages ? 0 : nr_pages);
}
/* ── pmm_alloc_pages ──────────────────────────── */
static void* pmm_alloc_pages(size_t count)
{
    if (count == 0)
        return NULL;
    if (count == 1) {
        uint64_t phys = pmm_alloc_frame();
        return phys ? (void *)PHYS_TO_VIRT(phys) : NULL;
    }
    uint64_t phys = (uint64_t)pmm_alloc_frames(count);
    if (unlikely(!phys))
        return NULL;
    return (void *)PHYS_TO_VIRT(phys);
}

/* ── pmm_free_pages ─────────────────────────────── */
static int pmm_free_pages(void *addr, size_t count)
{
    if (!addr || count == 0)
        return -EINVAL;
    uint64_t phys = VIRT_TO_PHYS((uint64_t)(uintptr_t)addr);
    if (phys & (PAGE_SIZE - 1))
        return -EINVAL;

    if (count == 1) {
        pmm_free_frame(phys);
    } else {
        pmm_free_frames_contiguous(phys, count);
    }
    return 0;
}

/* ── pmm_stats ─────────────────────────────── */
int pmm_stats(void *stats)
{
    if (!stats) return -EFAULT;
    struct {
        uint64_t total_frames;
        uint64_t used_frames;
        uint64_t free_frames;
        uint64_t largest_free_block;
        uint64_t free_block_count;
        uint64_t pgalloc;
        uint64_t pgfree;
    } st;

    st.total_frames      = total_frames;
    st.used_frames       = used_frames;
    st.free_frames       = (total_frames > used_frames) ? (total_frames - used_frames) : 0;
    st.largest_free_block = pmm_largest_free_block();
    st.free_block_count   = pmm_free_block_count();
    st.pgalloc            = vm_pgalloc;
    st.pgfree             = vm_pgfree;

    memcpy(stats, &st, sizeof(st));
    return 0;
}
