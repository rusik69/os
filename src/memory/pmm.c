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
int pmm_poison_enabled = 1;

/* ── Per-CPU page hot cache ────────────────────────────────────────────
 * Each CPU keeps a small pool of pre-allocated pages to avoid lock
 * contention on the global bitmap.  The hot cache is lock-free for the
 * owning CPU (only local IRQ save/restore is needed for reentrancy from
 * interrupt handlers on the same CPU).
 */
#define PMM_CPU_CACHE_SIZE 8

struct pmm_cpu_cache {
    uint64_t frames[PMM_CPU_CACHE_SIZE]; /* cached physical page addresses */
    int      count;                       /* number of valid entries */
};

/* One cache slot per possible CPU */
static struct pmm_cpu_cache pmm_cpu_cache[SMP_MAX_CPUS];

/* Global spinlock protecting the bitmap and shared counters during
 * cache refill/drain operations.  The fast per-CPU path avoids this. */
static spinlock_t pmm_global_lock;

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
    frame_bitmap[frame / 8] |= (1 << (frame % 8));
}

static void bitmap_clear(uint64_t frame) {
    if (frame >= MAX_FRAMES) return;
    frame_bitmap[frame / 8] &= ~(1 << (frame % 8));
}

static int bitmap_test(uint64_t frame) {
    if (frame >= MAX_FRAMES) return 1; /* out-of-range frames appear used */
    return frame_bitmap[frame / 8] & (1 << (frame % 8));
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

void pmm_init(uint64_t multiboot_info_phys) {
    /* Mark all frames as used initially */
    memset(frame_bitmap, 0xFF, sizeof(frame_bitmap));
    used_frames = MAX_FRAMES;

    struct multiboot_info *mbi = (struct multiboot_info *)PHYS_TO_VIRT(multiboot_info_phys);

    /* Check if memory map is available (bit 6 of flags) */
    if (mbi->flags & (1 << 6)) {
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

    /* Recount used frames based on actual total */
    used_frames = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        if (bitmap_test(f)) used_frames++;
    }

    /* Initialize the global spinlock for SMP-safe bitmap access */
    spinlock_init(&pmm_global_lock);

    /* Pre-populate the boot CPU's hot cache */
    pmm_cache_refill();

    kprintf("[OK] Physical Memory Manager: %llu frames (%llu MB), %llu free\n",
            (unsigned long long)total_frames,
            (unsigned long long)((total_frames * 4ULL) / 1024ULL),
            (unsigned long long)(total_frames - used_frames));
}

void pmm_reserve_frames(uint64_t phys_start, uint64_t byte_size) {
    uint64_t start_frame = phys_start / PAGE_SIZE;
    uint64_t end_frame   = (phys_start + byte_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (end_frame > MAX_FRAMES) end_frame = MAX_FRAMES;
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

/* ── Memory statistics dumping ──────────────────────────────────────────── */

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

    /* Scan bitmap to find the largest contiguous free block and count free runs */
    uint64_t max_run = 0, cur_run = 0;
    uint64_t free_runs = 0;
    int in_run = 0;
    for (uint64_t f = 0; f < total_frames; f++) {
        if (!bitmap_test(f)) {
            cur_run++;
            in_run = 1;
        } else {
            if (in_run) { free_runs++; in_run = 0; }
            if (cur_run > max_run) max_run = cur_run;
            cur_run = 0;
        }
    }
    if (in_run) { free_runs++; }
    if (cur_run > max_run) max_run = cur_run;

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

    /* Per-CPU hot cache occupancy */
    int total_cached = 0;
    for (int c = 0; c < smp_get_cpu_count(); c++)
        total_cached += pmm_cpu_cache[c].count;
    kprintf("[PMM] per-CPU caches: %d frames cached across %d CPUs\n",
            total_cached, smp_get_cpu_count());
}

/* ── Page allocator ─────────────────────────────────────────────────────── */

uint64_t pmm_alloc_frame(void) {
    int cpu = smp_get_cpu_id();
    struct pmm_cpu_cache *cache = &pmm_cpu_cache[cpu];

    /* ── Fast path: pop from per-CPU hot cache ── */
    /* Disable local IRQs to prevent reentrancy from interrupt handlers */
    uint64_t irq_save;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(irq_save) : : "memory");

    if (cache->count > 0) {
        cache->count--;
        uint64_t addr = cache->frames[cache->count];

        if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");
        poison_fill(addr, 0xDEADBEEF);
        vm_pgalloc++;
        return addr;
    }

    if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");

    /* ── Slow path: refill cache from global bitmap ── */
    pmm_cache_refill();

    /* Now try the cache again */
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(irq_save) : : "memory");
    if (cache->count > 0) {
        cache->count--;
        uint64_t addr = cache->frames[cache->count];
        if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");
        poison_fill(addr, 0xDEADBEEF);
        vm_pgalloc++;
        return addr;
    }
    if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");

    /* ── Out of memory: recovery level 1 — slab reaping + OOM killer ── */
    kprintf("[PMM] Out of memory! Attempting OOM recovery (slab reaping + OOM kill)...\n");

    kmem_cache_reap();
    oom_kill(1);
    scheduler_yield();

    pmm_cache_refill();

    __asm__ volatile("pushfq; pop %0; cli" : "=r"(irq_save) : : "memory");
    if (cache->count > 0) {
        cache->count--;
        uint64_t addr = cache->frames[cache->count];
        if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");
        poison_fill(addr, 0xDEADBEEF);
        vm_pgalloc++;
        return addr;
    }
    if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");

    /* ── Recovery level 2 — compaction + aggressive OOM ── */
    kprintf("[PMM] OOM recovery level 1 failed! Running compaction + aggressive OOM...\n");

    compaction_run();
    oom_kill(1);
    scheduler_yield();

    pmm_cache_refill();

    __asm__ volatile("pushfq; pop %0; cli" : "=r"(irq_save) : : "memory");
    if (cache->count > 0) {
        cache->count--;
        uint64_t addr = cache->frames[cache->count];
        if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");
        poison_fill(addr, 0xDEADBEEF);
        vm_pgalloc++;
        return addr;
    }
    if (irq_save & 0x200) __asm__ volatile("sti" : : : "memory");

    /* ── Final: panic with full memory diagnostics ── */
    pmm_dump_stats();
    panic("[PMM] Out of memory — OOM killer and compaction failed to reclaim any frames");
    /* unreachable */
    return 0;
}

/* Allocate count contiguous frames. Returns first frame physical addr, or 0 on failure. */
uint64_t *pmm_alloc_frames(size_t count) {
    if (count == 0) return NULL;
    if (count == 1) return (uint64_t *)pmm_alloc_frame();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&pmm_global_lock, &irq_flags);

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
                return (uint64_t *)(start * PAGE_SIZE);
            }
        } else {
            found = 0;
        }
        i++;
        if (i >= total_frames) i = 0;
    } while (i != pmm_hint);

    spinlock_irqsave_release(&pmm_global_lock, irq_flags);

    /* ── First recovery: slab reaping + OOM killer ── */
    kprintf("[PMM] Out of memory for %llu contiguous frames! Attempting OOM recovery...\n",
            (unsigned long long)count);

    kmem_cache_reap();
    oom_kill(1);
    scheduler_yield();

    /* Second attempt */
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
                return (uint64_t *)(start * PAGE_SIZE);
            }
        } else {
            found = 0;
        }
        i++;
        if (i >= total_frames) i = 0;
    } while (i != pmm_hint);
    spinlock_irqsave_release(&pmm_global_lock, irq_flags);

    /* ── Second recovery: compaction ── */
    kprintf("[PMM] OOM recovery for %llu contiguous frames failed! Running compaction...\n",
            (unsigned long long)count);

    compaction_run();
    oom_kill(1);
    scheduler_yield();

    /* Third attempt */
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
                return (uint64_t *)(start * PAGE_SIZE);
            }
        } else {
            found = 0;
        }
        i++;
        if (i >= total_frames) i = 0;
    } while (i != pmm_hint);
    spinlock_irqsave_release(&pmm_global_lock, irq_flags);

    /* ── Final: panic with full diagnostics ── */
    pmm_dump_stats();
    panic("[PMM] Out of memory — cannot allocate %llu contiguous frames",
          (unsigned long long)count);
    /* unreachable */
    return NULL;
}

void pmm_free_frame(uint64_t addr) {
    if (addr & (PAGE_SIZE - 1)) return;

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
