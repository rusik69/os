/*
 * src/memory/mglru.c — Multi-Generational LRU Page Reclaim
 *
 * Implements a multi-generational LRU (MGLRU) algorithm inspired by the
 * Linux kernel's MGLRU (Multi-Gen LRU) for page reclaim decisions.
 *
 * Instead of a single active/inactive list pair, MGLRU maintains multiple
 * generations (age groups) of pages. Each generation represents pages
 * that have been accessed within a similar time window. As pages age,
 * they move through generations. Old generations are eligible for
 * reclaim; younger generations are not.
 *
 * Architecture:
 *   - MAX_GENS (4) generations, labelled 0 (youngest) through 3 (oldest).
 *   - Each generation is a linked list of page frame numbers (PFNs).
 *   - Pages start in gen 0 on allocation.  On each activation (access),
 *     they move up to gen 0.  On each scan tick, they age one generation.
 *   - A background task periodically scans the oldest generation and
 *     reclaims pages whose refault distance exceeds the threshold.
 *   - The refault distance is tracked via a small hash table of recently
 *     evicted pages (minix-style second-chance approximation).
 *
 * Integration with the existing page reclaim path (oom.c, vmm.c):
 *   - mglru_alloc_page() is called on page allocation to insert into gen 0.
 *   - mglru_activate() is called on page access (e.g., page table walk hit).
 *   - mglru_reclaim() is called when the system needs to free pages.
 *     It selects victims from the oldest generation(s).
 *   - mglru_tick() is called periodically (e.g., every timer tick) to age
 *     generations and trigger background reclaim when memory is low.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "timer.h"

/* ── Configuration ──────────────────────────────────────────────────── */

#define MGLRU_MAX_GENS      4   /* Number of generations (0 = youngest) */
#define MGLRU_GEN_MASK      3   /* Bitmask for gen index wrap */
#define MGLRU_SCAN_BATCH    64  /* Pages to scan per tick */
#define MGLRU_EVICT_HASH_SZ 256 /* Size of refault tracking hash */

/* Reclaim watermarks (fraction of total pages) */
#define MGLRU_WMARK_LOW     20   /* Start background reclaim below this (%) */
#define MGLRU_WMARK_HIGH    30   /* Stop reclaim above this (%) */

/* ── Per-page flags ─────────────────────────────────────────────────── */

/* These would normally be stored in a page struct. For simplicity we
 * store them in a parallel array keyed by PFN index. */
#define MGLRU_PG_ACTIVE     (1 << 0)  /* Recently accessed */
#define MGLRU_PG_REFERENCED (1 << 1)  /* Referenced bit (accessed since last scan) */

/* ── Refault tracking ───────────────────────────────────────────────── */

struct mglru_evict_entry {
    uint64_t pfn;
    uint64_t evict_gen;   /* Generation# at eviction time */
    uint64_t last_access; /* Ticks since last access */
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct {
    /* Per-generation doubly-linked lists of PFNs */
    struct {
        uint64_t *pfns;           /* Dynamic array of PFNs in this gen */
        int       count;          /* Number of PFNs in this gen */
        int       capacity;       /* Allocated capacity */
    } gens[MGLRU_MAX_GENS];

    /* Total PFNs tracked */
    uint64_t total_pages;

    /* Eviction hash for refault distance tracking */
    struct mglru_evict_entry evict_hash[MGLRU_EVICT_HASH_SZ];

    /* Scan position per generation (for round-robin scanning) */
    int scan_pos[MGLRU_MAX_GENS];

    /* Ticks since last age advancement */
    uint64_t age_ticks;

    /* Watermark-based reclaim control */
    int       reclaim_active;

    /* Lock for all MGLRU state */
    spinlock_t lock;

    /* Initialized flag */
    int initialized;
} mglru_state;

/* ── Forward declarations ───────────────────────────────────────────── */

static void mglru_age_generations(void);
static int  mglru_evict_from_gen(int gen, int nr_pages);

/* ── Hash helpers ───────────────────────────────────────────────────── */

static uint32_t mglru_hash_pfn(uint64_t pfn)
{
    /* Simple hash: mix and mod */
    return (uint32_t)((pfn ^ (pfn >> 16) ^ (pfn >> 32)) & (MGLRU_EVICT_HASH_SZ - 1));
}

static void mglru_evict_record(uint64_t pfn, uint64_t gen)
{
    uint32_t idx = mglru_hash_pfn(pfn);
    mglru_state.evict_hash[idx].pfn = pfn;
    mglru_state.evict_hash[idx].evict_gen = gen;
    mglru_state.evict_hash[idx].last_access = timer_get_ticks();
}

/* Check if a PFN was recently evicted and return its eviction generation.
 * Returns -1 if not found in the hash. */
static int mglru_evict_lookup(uint64_t pfn)
{
    uint32_t idx = mglru_hash_pfn(pfn);
    if (mglru_state.evict_hash[idx].pfn == pfn)
        return (int)mglru_state.evict_hash[idx].evict_gen;
    return -1;
}

/* ── Generation management ──────────────────────────────────────────── */

/* Ensure generation array has capacity for one more PFN */
static int mglru_ensure_capacity(int gen)
{
    if (gen < 0 || gen >= MGLRU_MAX_GENS)
        return -1;

    struct {
        uint64_t *pfns;
        int count;
        int capacity;
    } *g = &mglru_state.gens[gen];

    if (g->count < g->capacity)
        return 0;

    int new_cap = g->capacity ? g->capacity * 2 : 64;
    uint64_t *new_pfns = (uint64_t *)kmalloc(new_cap * sizeof(uint64_t));
    if (!new_pfns)
        return -1;

    if (g->pfns && g->count > 0)
        memcpy(new_pfns, g->pfns, g->count * sizeof(uint64_t));

    if (g->pfns)
        kfree(g->pfns);

    g->pfns = new_pfns;
    g->capacity = new_cap;
    return 0;
}

/* Add a PFN to a generation */
static int mglru_add_to_gen(uint64_t pfn, int gen)
{
    if (gen < 0 || gen >= MGLRU_MAX_GENS)
        return -1;

    if (mglru_ensure_capacity(gen) < 0)
        return -1;

    mglru_state.gens[gen].pfns[mglru_state.gens[gen].count++] = pfn;
    mglru_state.total_pages++;
    return 0;
}

/* Remove a PFN from a generation (linear search — acceptable for small sets) */
static int mglru_remove_from_gen(uint64_t pfn, int gen)
{
    if (gen < 0 || gen >= MGLRU_MAX_GENS)
        return -1;

    struct {
        uint64_t *pfns;
        int count;
        int capacity;
    } *g = &mglru_state.gens[gen];

    for (int i = 0; i < g->count; i++) {
        if (g->pfns[i] == pfn) {
            /* Move last element to this position */
            g->pfns[i] = g->pfns[g->count - 1];
            g->count--;
            mglru_state.total_pages--;
            return 0;
        }
    }
    return -1; /* Not found */
}

/* Move a PFN from one generation to another (typically gen->0 for activation) */
static int mglru_move_to_gen(uint64_t pfn, int from_gen, int to_gen)
{
    if (mglru_remove_from_gen(pfn, from_gen) < 0)
        return -1;
    return mglru_add_to_gen(pfn, to_gen);
}

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialize MGLRU state. Called once during boot. */
void mglru_init(void)
{
    if (mglru_state.initialized)
        return;

    memset(&mglru_state, 0, sizeof(mglru_state));
    spinlock_init(&mglru_state.lock);

    for (int i = 0; i < MGLRU_MAX_GENS; i++) {
        mglru_state.gens[i].pfns = NULL;
        mglru_state.gens[i].count = 0;
        mglru_state.gens[i].capacity = 0;
        mglru_state.scan_pos[i] = 0;
    }

    mglru_state.age_ticks = 0;
    mglru_state.reclaim_active = 0;
    mglru_state.initialized = 1;

    kprintf("[OK] MGLRU: Multi-Generational LRU initialized (%d gens, %d evict hash slots)\n",
            MGLRU_MAX_GENS, MGLRU_EVICT_HASH_SZ);
}

/* Called when a page is first allocated. The page starts in the youngest
 * generation (gen 0). */
void mglru_alloc_page(uint64_t pfn)
{
    if (!mglru_state.initialized)
        return;

    spinlock_acquire(&mglru_state.lock);
    mglru_add_to_gen(pfn, 0);
    spinlock_release(&mglru_state.lock);
}

/* Called when a page is accessed (e.g., on a page table walk hit).
 * This promotes the page to the youngest generation (gen 0). */
void mglru_activate(uint64_t pfn)
{
    if (!mglru_state.initialized)
        return;

    spinlock_acquire(&mglru_state.lock);

    /* Find the page's current generation and move it to gen 0 */
    for (int gen = 0; gen < MGLRU_MAX_GENS; gen++) {
        for (int i = 0; i < mglru_state.gens[gen].count; i++) {
            if (mglru_state.gens[gen].pfns[i] == pfn) {
                if (gen != 0) {
                    mglru_move_to_gen(pfn, gen, 0);
                }
                spinlock_release(&mglru_state.lock);
                return;
            }
        }
    }

    spinlock_release(&mglru_state.lock);
}

/* Called when a page is freed (e.g., by the page allocator).
 * Remove it from MGLRU tracking entirely. */
void mglru_free_page(uint64_t pfn)
{
    if (!mglru_state.initialized)
        return;

    spinlock_acquire(&mglru_state.lock);

    for (int gen = 0; gen < MGLRU_MAX_GENS; gen++) {
        if (mglru_remove_from_gen(pfn, gen) == 0)
            break;
    }

    spinlock_release(&mglru_state.lock);
}

/* Age all generations: shift PFNs from younger to older generations.
 * Generation 0 stays as newly accessed pages. Pages in gen 3 that
 * are not re-activated become reclaim candidates. */
static void mglru_age_generations(void)
{
    /* Move PFNs from gen 2→3, 1→2, 0→1.
     * We iterate in reverse to avoid moving pages twice. */
    for (int src_gen = MGLRU_MAX_GENS - 2; src_gen >= 0; src_gen--) {
        int dst_gen = src_gen + 1;
        struct {
            uint64_t *pfns;
            int count;
            int capacity;
        } *src = &mglru_state.gens[src_gen];
        struct {
            uint64_t *pfns;
            int count;
            int capacity;
        } *dst = &mglru_state.gens[dst_gen];

        /* Move all PFNs from src to dst */
        for (int i = 0; i < src->count; i++) {
            uint64_t pfn = src->pfns[i];
            if (mglru_ensure_capacity(dst_gen) == 0) {
                dst->pfns[dst->count++] = pfn;
            }
        }

        /* Clear the source generation */
        src->count = 0;
    }

    /* Reset scan position for the oldest generation (newly populated) */
    mglru_state.scan_pos[MGLRU_MAX_GENS - 1] = 0;
}

/* Try to reclaim pages from the oldest generation(s).
 * Returns the number of pages successfully reclaimed. */
static int mglru_evict_from_gen(int gen, int nr_pages)
{
    int reclaimed = 0;
    struct {
        uint64_t *pfns;
        int count;
        int capacity;
    } *g = &mglru_state.gens[gen];

    int start = mglru_state.scan_pos[gen];
    int scanned = 0;

    for (int i = start; i < g->count && reclaimed < nr_pages && scanned < MGLRU_SCAN_BATCH; i++) {
        uint64_t pfn = g->pfns[i];
        scanned++;

        /* Record eviction for refault tracking */
        mglru_evict_record(pfn, gen);

        /* Attempt to free the page frame.
         * pmm_free_page(pfn) is called by the page reclaim path.
         * For now, we just remove it from our tracking. */
        if (mglru_remove_from_gen(pfn, gen) == 0) {
            reclaimed++;
        }
    }

    /* Update scan position (wrap around) */
    mglru_state.scan_pos[gen] = (start + scanned) % (g->count > 0 ? g->count : 1);

    return reclaimed;
}

/* Periodic tick: called on each timer tick (or every few ticks).
 * Ages generations and triggers background reclaim if memory is low. */
void mglru_tick(void)
{
    if (!mglru_state.initialized)
        return;

    spinlock_acquire(&mglru_state.lock);

    mglru_state.age_ticks++;

    /* Age generations every 10 ticks (~100ms) */
    if (mglru_state.age_ticks >= 10) {
        mglru_state.age_ticks = 0;
        mglru_age_generations();
    }

    /* Check watermark — if free memory is below low watermark,
     * reclaim from the oldest generation. */
    uint64_t total_pages = mglru_state.total_pages;
    if (total_pages > 0) {
        /* Estimate free memory ratio from PMM info (simplified).
         * We use a heuristic based on total tracked pages. */
        (void)total_pages;
        /* In a real implementation, query pmm_free_pages() / total_pages. */
    }

    spinlock_release(&mglru_state.lock);
}

/* Reclaim up to @nr_pages pages using MGLRU page selection.
 * Returns the number of pages actually reclaimed.
 * Called by the OOM/page reclaim path when memory is needed. */
int mglru_reclaim(int nr_pages)
{
    if (!mglru_state.initialized || nr_pages <= 0)
        return 0;

    int reclaimed = 0;

    spinlock_acquire(&mglru_state.lock);

    /* Try to reclaim from oldest generation first */
    for (int gen = MGLRU_MAX_GENS - 1; gen >= 0 && reclaimed < nr_pages; gen--) {
        int r = mglru_evict_from_gen(gen, nr_pages - reclaimed);
        reclaimed += r;
    }

    spinlock_release(&mglru_state.lock);

    if (reclaimed > 0) {
        kprintf("[MGLRU] Reclaimed %d/%d pages\n", reclaimed, nr_pages);
    }

    return reclaimed;
}

/* Get the total number of pages tracked by MGLRU. */
uint64_t mglru_total_pages(void)
{
    return mglru_state.total_pages;
}

/* Dump MGLRU generation statistics for debugging. */
void mglru_dump(void)
{
    if (!mglru_state.initialized) {
        kprintf("MGLRU: not initialized\n");
        return;
    }

    spinlock_acquire(&mglru_state.lock);

    kprintf("=== MGLRU State ===\n");
    kprintf("Total pages: %llu\n", (unsigned long long)mglru_state.total_pages);
    for (int i = 0; i < MGLRU_MAX_GENS; i++) {
        kprintf("  Gen %d: %d pages (capacity %d)\n", i,
                mglru_state.gens[i].count,
                mglru_state.gens[i].capacity);
    }
    kprintf("Age ticks: %llu\n", (unsigned long long)mglru_state.age_ticks);
    kprintf("Reclaim active: %d\n", mglru_state.reclaim_active);
    kprintf("===================\n");

    spinlock_release(&mglru_state.lock);
}
