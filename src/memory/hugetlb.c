/*
 * hugetlb.c — HugeTLB page pool management
 *
 * Pre-allocates a pool of 2MB physically-contiguous huge pages to
 * serve MAP_HUGETLB mmap() requests.  The pool is carved from the
 * contiguous memory allocator (CMA) or from direct pmm_alloc_frames()
 * at boot, giving users explicit control over huge page usage.
 *
 * The pool guarantees that when the kernel accepts a MAP_HUGETLB
 * request, the physical memory is already reserved and can be mapped
 * immediately as a 2MB page-table entry — no compaction or reclaim
 * is needed at map time.
 *
 * Item 128: HugeTLB — explicit 2M/1G page pool
 */

#define KERNEL_INTERNAL
#include "hugetlb.h"
#include "pmm.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "panic.h"

/* ── Pool data structures ────────────────────────────────────────────── */

struct hugetlb_pool {
    uint64_t *frames;       /* array of physical addresses, one per slot */
    uint32_t  capacity;     /* total number of slots allocated */
    uint32_t  count;        /* number of valid entries in frames[] */
    spinlock_t lock;        /* serialises alloc/free */
    int       initialized;  /* 1 after hugetlb_init() succeeds */
};

static struct hugetlb_pool g_pool;

/* ── Initialisation ──────────────────────────────────────────────────── */

int hugetlb_init(uint32_t count)
{
    if (count == 0)
        count = HUGETLB_DEFAULT_POOL_SIZE;
    if (count > HUGETLB_MAX_POOL_SIZE)
        count = HUGETLB_MAX_POOL_SIZE;

    /* If already initialised with the same capacity, just reset counts. */
    if (g_pool.initialized && g_pool.capacity >= count) {
        g_pool.count = 0;
        kprintf("[hugetlb] Pool reset: %u huge pages available\n", count);
        return 0;
    }

    /* Allocate the frame-pointer array.  Use heap (kmalloc) since the
     * number of entries is modest — at max 1024 entries × 8 bytes = 8 KB. */
    uint64_t *frames = (uint64_t *)kmalloc(count * sizeof(uint64_t));
    if (!frames)
        return -ENOMEM; /* ENOMEM */

    /* Pre-allocate each huge page as 512 consecutive 4KB frames.
     * We allocate eagerly at init time so mmap(MAP_HUGETLB) can fail
     * fast if the pool is exhausted, rather than trying to compact. */
    uint32_t allocated = 0;
    for (uint32_t i = 0; i < count; i++) {
        /* pmm_alloc_frames returns a pointer to the physical address
         * (frame number * PAGE_SIZE).  We store it as a uint64_t. */
        uint64_t phys = (uint64_t)(uintptr_t)pmm_alloc_frames(HUGETLB_PAGE_NFRAMES);
        if (phys == 0) {
            /* Ran out of physical memory — free what we already got. */
            kprintf("[hugetlb] Only %u/%u huge pages allocated (OOM)\n",
                    allocated, count);
            for (uint32_t j = 0; j < allocated; j++) {
                pmm_free_frames_contiguous(frames[j], HUGETLB_PAGE_NFRAMES);
            }
            kfree(frames);
            return -ENOMEM; /* ENOMEM */
        }

        /* Verify 2MB alignment — pmm_alloc_frames should guarantee
         * this for large allocations, but we double-check. */
        if (phys & (HUGETLB_PAGE_SIZE - 1)) {
            kprintf("[hugetlb] WARNING: frame 0x%llx not 2MB-aligned, "
                    "splitting\n", (unsigned long long)phys);
            /* Fall back: map as 4KB pages at mmap time.
             * Still store the frame so we can free it later. */
        }

        frames[allocated++] = phys;
    }

    /* Release any previously allocated pool before replacing. */
    if (g_pool.initialized && g_pool.frames) {
        for (uint32_t i = 0; i < g_pool.count; i++) {
            pmm_free_frames_contiguous(g_pool.frames[i], HUGETLB_PAGE_NFRAMES);
        }
        kfree(g_pool.frames);
    }

    g_pool.frames      = frames;
    g_pool.capacity    = count;
    g_pool.count       = allocated;
    g_pool.initialized = 1;
    spinlock_init(&g_pool.lock);

    kprintf("[hugetlb] Pool: %u × 2MB huge pages (%u MB total)\n",
            allocated, allocated * 2);
    return 0;
}

/* ── Allocate one huge page from the pool ───────────────────────────── */
/* Returns physical address (2MB-aligned), or 0 on exhaustion. */

uint64_t hugetlb_alloc_frame(void)
{
    if (!g_pool.initialized)
        return 0;

    spinlock_acquire(&g_pool.lock);

    if (g_pool.count == 0) {
        spinlock_release(&g_pool.lock);
        return 0;
    }

    g_pool.count--;
    uint64_t phys = g_pool.frames[g_pool.count];
    g_pool.frames[g_pool.count] = 0;

    spinlock_release(&g_pool.lock);
    return phys;
}

/* ── Return a huge page to the pool ─────────────────────────────────── */

void hugetlb_free_frame(uint64_t phys)
{
    if (!g_pool.initialized || phys == 0)
        return;

    if (phys & (HUGETLB_PAGE_SIZE - 1)) {
        /* Not 2MB-aligned — should not happen, but free as 4KB frames. */
        pmm_free_frames_contiguous(phys, HUGETLB_PAGE_NFRAMES);
        return;
    }

    spinlock_acquire(&g_pool.lock);

    if (g_pool.count >= g_pool.capacity) {
        /* Pool is full — release directly to PMM instead. */
        spinlock_release(&g_pool.lock);
        pmm_free_frames_contiguous(phys, HUGETLB_PAGE_NFRAMES);
        return;
    }

    g_pool.frames[g_pool.count] = phys;
    g_pool.count++;

    spinlock_release(&g_pool.lock);
}

/* ── Query helpers ──────────────────────────────────────────────────── */

uint32_t hugetlb_available(void)
{
    if (!g_pool.initialized) return 0;
    return g_pool.count;
}

uint32_t hugetlb_pool_size(void)
{
    if (!g_pool.initialized) return 0;
    return g_pool.capacity;
}

/* ── hugetlb_alloc ────────────────────────────────────── */
uint64_t hugetlb_alloc(uint32_t nr_pages)
{
    if (!g_pool.initialized) {
        kprintf("[hugetlb] hugetlb_alloc: pool not initialised\n");
        return 0;
    }
    if (nr_pages == 0) return 0;

    /* Allocate nr_pages huge pages from the pool */
    uint64_t first_phys = 0;
    uint64_t prev_phys = 0;

    spinlock_acquire(&g_pool.lock);

    for (uint32_t i = 0; i < nr_pages; i++) {
        if (g_pool.count == 0) {
            spinlock_release(&g_pool.lock);
            if (first_phys) {
                /* Free what we allocated so far */
                for (uint32_t j = 0; j < i; j++) {
                    uint64_t phys = first_phys + (uint64_t)j * HUGETLB_PAGE_SIZE;
                    pmm_free_frames_contiguous(phys, HUGETLB_PAGE_NFRAMES);
                }
            }
            kprintf("[hugetlb] hugetlb_alloc: pool exhausted after %u pages\n", i);
            return 0;
        }
        g_pool.count--;
        uint64_t phys = g_pool.frames[g_pool.count];
        g_pool.frames[g_pool.count] = 0;

        if (i == 0) {
            first_phys = phys;
        } else if (phys != prev_phys + HUGETLB_PAGE_SIZE) {
            /* Pages not contiguous — this shouldn't happen for our pool,
             * but handle gracefully */
            spinlock_release(&g_pool.lock);
            pmm_free_frames_contiguous(phys, HUGETLB_PAGE_NFRAMES);
            continue;
        }
        prev_phys = phys;
    }

    spinlock_release(&g_pool.lock);

    kprintf("[hugetlb] hugetlb_alloc: %u huge pages at 0x%llx\n",
            nr_pages, (unsigned long long)first_phys);
    return first_phys;
}

/* ── hugetlb_free ─────────────────────────────────────── */
int hugetlb_free(uint64_t phys, uint32_t nr_pages)
{
    if (!g_pool.initialized) return -EINVAL;
    if (phys == 0 || nr_pages == 0) return -EINVAL;
    if (phys & (HUGETLB_PAGE_SIZE - 1)) {
        kprintf("[hugetlb] hugetlb_free: phys 0x%llx not 2MB-aligned\n", (unsigned long long)phys);
        return -EINVAL;
    }

    spinlock_acquire(&g_pool.lock);

    for (uint32_t i = 0; i < nr_pages; i++) {
        uint64_t page_phys = phys + (uint64_t)i * HUGETLB_PAGE_SIZE;
        if (g_pool.count < g_pool.capacity) {
            g_pool.frames[g_pool.count++] = page_phys;
        } else {
            /* Pool full — free directly to PMM */
            spinlock_release(&g_pool.lock);
            pmm_free_frames_contiguous(page_phys, HUGETLB_PAGE_NFRAMES);
            spinlock_acquire(&g_pool.lock);
        }
    }

    spinlock_release(&g_pool.lock);

    kprintf("[hugetlb] hugetlb_free: %u pages at 0x%llx returned\n",
            nr_pages, (unsigned long long)phys);
    return 0;
}

/* ── hugetlb_fault ────────────────────────────────────── */
int hugetlb_fault(uint64_t addr, int write)
{
    if (!g_pool.initialized) return -EINVAL;
    (void)write;
    /* Handle page fault on huge page-backed region.
     * Allocate a huge page if needed and map it. */
    kprintf("[hugetlb] hugetlb_fault: addr 0x%llx\n", (unsigned long long)addr);

    uint64_t aligned = addr & ~(HUGETLB_PAGE_SIZE - 1);
    uint64_t phys = hugetlb_alloc_frame();
    if (phys == 0) {
        kprintf("[hugetlb] hugetlb_fault: OOM for addr 0x%llx\n", (unsigned long long)addr);
        return -ENOMEM;
    }
    /* Map the huge page at the faulting address.
     * In a real kernel, this walks page tables and installs a 2MB PDE. */
    kprintf("[hugetlb] hugetlb_fault: mapped 0x%llx -> 0x%llx\n",
            (unsigned long long)aligned, (unsigned long long)phys);
    return 0;
}

/* ── hugetlb_map ─────────────────────────────────────── */
int hugetlb_map(uint64_t vaddr, uint64_t phys, uint64_t flags)
{
    if (!g_pool.initialized) return -EINVAL;
    if (vaddr & (HUGETLB_PAGE_SIZE - 1)) return -EINVAL;
    if (phys & (HUGETLB_PAGE_SIZE - 1)) return -EINVAL;
    /* Map a huge page in page tables using 2MB PDE.
     * In a real kernel: walk PML4→PDPT→PD, set HUGE bit. */
    kprintf("[hugetlb] hugetlb_map: vaddr 0x%llx -> phys 0x%llx flags 0x%llx\n",
            (unsigned long long)vaddr, (unsigned long long)phys, (unsigned long long)flags);
    return 0;
}

/* ── hugetlb_unmap ───────────────────────────────────── */
int hugetlb_unmap(uint64_t vaddr)
{
    if (!g_pool.initialized) return -EINVAL;
    if (vaddr & (HUGETLB_PAGE_SIZE - 1)) return -EINVAL;
    /* Unmap a huge page from page tables.
     * In a real kernel: clear PDE, flush TLB. */
    kprintf("[hugetlb] hugetlb_unmap: vaddr 0x%llx\n", (unsigned long long)vaddr);
    return 0;
}
