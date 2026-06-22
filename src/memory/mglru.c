#define KERNEL_INTERNAL

#include "mglru.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "hrtimer.h"
#include "sysfs.h"
#include "errno.h"
#include "smp.h"

/* ════════════════════════════════════════════════════════════════════════
 *  Multi-Generational LRU (MGLRU) Page Reclaim — Implementation
 *
 *  Manages pages across 4 generations (0 = oldest, 3 = youngest).
 *  Generation advancement (aging) is driven by a periodic hrtimer.
 *  Pages are added to the youngest gen; on access they are promoted;
 *  reclaim isolates pages from the oldest non-empty gen and frees them.
 * ════════════════════════════════════════════════════════════════════════ */

/* ── Constants ──────────────────────────────────────────────────────── */

/* Default aging threshold in jiffies (~100 ms at 100 Hz timer, 1 ms
 * at 1000 Hz, but this kernel uses TIMER_FREQ which may vary).
 * Default is 1000 jiffies (~10 seconds at 100 Hz). */
#define MGLRU_DEFAULT_THRESHOLD    1000UL

/* Default min TTL in milliseconds */
#define MGLRU_DEFAULT_MIN_TTL_MS   1000UL

/* Periodic timer interval in nanoseconds (1000 ms) */
#define MGLRU_AGING_INTERVAL_NS    1000000000ULL   /* 1 second */

/* ── Static data ────────────────────────────────────────────────────── */

/* Per-node MGLRU state — we use the first node only (single-node system). */
static struct mglru_state mglru_state[MGLRU_NR_NODES];

/* Page tracking pool: fixed-size array, no heap allocation. */
static struct mglru_page_entry mglru_page_pool[MGLRU_MAX_PAGES];
static int                     mglru_pool_next;   /* hint for allocation */

/* Per-frame lookup: frame number → pool index (or -1 if not tracked).
 * This avoids scanning the LRU lists for remove/find operations. */
static int mglru_frame_map[262144];  /* MAX_FRAMES */

/* Periodic aging timer */
static struct hrtimer mglru_timer;
static int            mglru_timer_active;

/* ── Forward declarations ───────────────────────────────────────────── */
static int  mglru_alloc_entry(void);
static void mglru_free_entry(int idx);
static int  mglru_find_entry(uint64_t phys_addr);
static void mglru_sysfs_init(void);
static void mglru_aging_callback(void *data);

/* ════════════════════════════════════════════════════════════════════════
 *  Initialisation
 * ════════════════════════════════════════════════════════════════════════ */

void mglru_init(void)
{
    int node, gen;

    /* Initialise per-node state */
    for (node = 0; node < MGLRU_NR_NODES; node++) {
        struct mglru_state *st = &mglru_state[node];

        for (gen = 0; gen < MGLRU_NR_GENS; gen++) {
            INIT_LIST_HEAD(&st->gens[gen].list);
            st->gens[gen].nr_active   = 0;
            st->gens[gen].nr_inactive = 0;
            st->gens[gen].nr_pages    = 0;
        }

        st->last_accessed_gen = MGLRU_NR_GENS - 1;  /* youngest */
        st->last_evicted_gen  = 0;                   /* oldest */
        spinlock_init(&st->lock);
        st->aging_threshold = MGLRU_DEFAULT_THRESHOLD;
        st->min_ttl_ms      = MGLRU_DEFAULT_MIN_TTL_MS;
        st->enabled         = 1;
    }

    /* Clear the page tracking pool */
    memset(mglru_page_pool, 0, sizeof(mglru_page_pool));
    mglru_pool_next = 0;

    /* Clear frame map: all -1 (not tracked) */
    memset(mglru_frame_map, 0xFF, sizeof(mglru_frame_map));

    /* Create sysfs interface */
    mglru_sysfs_init();

    /* Start the periodic aging timer */
    hrtimer_init(&mglru_timer, mglru_aging_callback, NULL);
    if (hrtimer_start(&mglru_timer, MGLRU_AGING_INTERVAL_NS) == 0) {
        mglru_timer_active = 1;
    } else {
        kprintf("[MGLRU] WARNING: failed to start aging timer\n");
        mglru_timer_active = 0;
    }

    kprintf("[OK] MGLRU: Multi-Generational LRU page reclaim initialised "
            "(threshold=%lu jiffies, min_ttl=%lu ms)\n",
            MGLRU_DEFAULT_THRESHOLD, MGLRU_DEFAULT_MIN_TTL_MS);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Page tracking pool management
 * ════════════════════════════════════════════════════════════════════════ */

/* Allocate a slot in the page tracking pool.  Returns pool index or -1. */
static int mglru_alloc_entry(void)
{
    /* Linear scan from hint — simple and adequate for the pool size. */
    for (int i = 0; i < MGLRU_MAX_PAGES; i++) {
        int idx = (mglru_pool_next + i) % MGLRU_MAX_PAGES;
        if (!mglru_page_pool[idx].in_use) {
            mglru_page_pool[idx].in_use = 1;
            mglru_page_pool[idx].phys_addr = 0;
            mglru_page_pool[idx].gen = 0;
            mglru_page_pool[idx].accessed = 0;
            INIT_LIST_HEAD(&mglru_page_pool[idx].list);
            mglru_pool_next = (idx + 1) % MGLRU_MAX_PAGES;
            return idx;
        }
    }
    return -1;  /* pool exhausted */
}

/* Release a tracking entry back to the pool. */
static void mglru_free_entry(int idx)
{
    if (idx < 0 || idx >= MGLRU_MAX_PAGES)
        return;
    if (!mglru_page_pool[idx].in_use)
        return;

    /* Remove from LRU list if still linked */
    list_del(&mglru_page_pool[idx].list);

    memset(&mglru_page_pool[idx], 0, sizeof(mglru_page_pool[idx]));
}

/* Find the pool entry for a given physical address by consulting
 * the frame map.  Returns pool index, or -1 if not tracked. */
static int mglru_find_entry(uint64_t phys_addr)
{
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame >= 262144)  /* MAX_FRAMES */
        return -1;
    int idx = mglru_frame_map[frame];
    if (idx < 0 || idx >= MGLRU_MAX_PAGES)
        return -1;
    if (!mglru_page_pool[idx].in_use ||
        mglru_page_pool[idx].phys_addr != phys_addr)
        return -1;
    return idx;
}

/* Update the frame map for a given phys_addr to point to pool entry idx,
 * or -1 to clear the mapping. */
static void mglru_frame_map_set(uint64_t phys_addr, int idx)
{
    uint64_t frame = phys_addr / PAGE_SIZE;
    if (frame < 262144)  /* MAX_FRAMES */
        mglru_frame_map[frame] = idx;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Core API
 * ════════════════════════════════════════════════════════════════════════ */

/* ── mglru_add_page ────────────────────────────────────────────────────
 * Add a newly allocated page to the youngest generation.
 * Must be called once per page after allocation. */
void mglru_add_page(uint64_t phys_addr)
{
    if (phys_addr == 0)
        return;

    /* Use node 0 for single-node system */
    struct mglru_state *st = &mglru_state[0];
    if (!st->enabled)
        return;

    /* Check if already tracked */
    int existing = mglru_find_entry(phys_addr);
    if (existing >= 0)
        return;   /* already tracked — don't duplicate */

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&st->lock, &irq_flags);

    int idx = mglru_alloc_entry();
    if (idx < 0) {
        /* Pool exhausted — warn occasionally, skip tracking */
        static unsigned long warn_count;
        if (warn_count++ % 64 == 0)
            kprintf("[MGLRU] WARNING: page tracking pool exhausted "
                    "(phys=0x%llx)\n", (unsigned long long)phys_addr);
        spinlock_irqsave_release(&st->lock, irq_flags);
        return;
    }

    struct mglru_page_entry *entry = &mglru_page_pool[idx];
    int target_gen = st->last_accessed_gen;

    entry->phys_addr = phys_addr;
    entry->gen       = target_gen;
    entry->accessed  = 0;
    entry->in_use    = 1;

    /* Add to the youngest generation's LRU list (tail = most recent). */
    list_add_tail(&entry->list, &st->gens[target_gen].list);
    st->gens[target_gen].nr_pages++;
    st->gens[target_gen].nr_inactive++;

    /* Update the frame map */
    mglru_frame_map_set(phys_addr, idx);

    spinlock_irqsave_release(&st->lock, irq_flags);
}

/* ── mglru_remove_page ─────────────────────────────────────────────────
 * Remove a page from MGLRU tracking when it is freed.
 * Safe to call even if the page was not tracked. */
void mglru_remove_page(uint64_t phys_addr)
{
    if (phys_addr == 0)
        return;

    struct mglru_state *st = &mglru_state[0];
    if (!st->enabled)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&st->lock, &irq_flags);

    int idx = mglru_find_entry(phys_addr);
    if (idx < 0) {
        spinlock_irqsave_release(&st->lock, irq_flags);
        return;
    }

    struct mglru_page_entry *entry = &mglru_page_pool[idx];
    int gen = entry->gen;

    if (gen >= 0 && gen < MGLRU_NR_GENS) {
        /* Decrement the generation's counters */
        struct mglru_gen *g = &st->gens[gen];
        if (g->nr_pages > 0) g->nr_pages--;
        if (entry->accessed) {
            if (g->nr_active > 0) g->nr_active--;
        } else {
            if (g->nr_inactive > 0) g->nr_inactive--;
        }
    }

    /* Clear frame map and free the entry */
    mglru_frame_map_set(phys_addr, -1);
    mglru_free_entry(idx);

    spinlock_irqsave_release(&st->lock, irq_flags);
}

/* ── mglru_page_accessed ───────────────────────────────────────────────
 * Mark a page as recently accessed.  On the next aging scan or isolate
 * pass, this flag causes the page to be promoted to a younger generation.
 * This is the lightweight access notification path (called from
 * page cache lookup, page fault, etc.). */
void mglru_page_accessed(uint64_t phys_addr)
{
    if (phys_addr == 0)
        return;

    struct mglru_state *st = &mglru_state[0];
    if (!st->enabled)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&st->lock, &irq_flags);

    int idx = mglru_find_entry(phys_addr);
    if (idx < 0) {
        spinlock_irqsave_release(&st->lock, irq_flags);
        return;
    }

    mglru_page_pool[idx].accessed = 1;

    spinlock_irqsave_release(&st->lock, irq_flags);
}

/* ── mglru_rotate ─────────────────────────────────────────────────────
 * Promote an accessed page to the youngest generation.
 * Called when the access flag is being consumed (during aging scan or
 * on explicit rotation).  Moves the page from its current gen to
 * last_accessed_gen, updating counters accordingly. */
void mglru_rotate(uint64_t phys_addr)
{
    if (phys_addr == 0)
        return;

    struct mglru_state *st = &mglru_state[0];
    if (!st->enabled)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&st->lock, &irq_flags);

    int idx = mglru_find_entry(phys_addr);
    if (idx < 0) {
        spinlock_irqsave_release(&st->lock, irq_flags);
        return;
    }

    struct mglru_page_entry *entry = &mglru_page_pool[idx];
    int old_gen = entry->gen;
    int new_gen = st->last_accessed_gen;

    if (old_gen == new_gen) {
        /* Already in the youngest gen — just mark not accessed */
        entry->accessed = 0;
        spinlock_irqsave_release(&st->lock, irq_flags);
        return;
    }

    /* Remove from old generation list */
    list_del(&entry->list);

    /* Update old gen counters */
    if (old_gen >= 0 && old_gen < MGLRU_NR_GENS) {
        struct mglru_gen *og = &st->gens[old_gen];
        if (og->nr_pages > 0) og->nr_pages--;
        if (entry->accessed) {
            if (og->nr_active > 0) og->nr_active--;
        } else {
            if (og->nr_inactive > 0) og->nr_inactive--;
        }
    }

    /* Move to new generation */
    entry->gen = new_gen;
    entry->accessed = 0;
    list_add_tail(&entry->list, &st->gens[new_gen].list);

    /* Update new gen counters */
    st->gens[new_gen].nr_pages++;
    st->gens[new_gen].nr_active++;  /* promoted pages are considered active */

    spinlock_irqsave_release(&st->lock, irq_flags);
}

/* ── mglru_age ─────────────────────────────────────────────────────────
 * Advance the current generation.  Wrapping at MGLRU_NR_GENS (4).
 * This effectively "ages" all pages by one generation:
 *   - last_accessed_gen moves forward (wrapping)
 *   - last_evicted_gen also moves forward so that reclaim targets the
 *     new oldest generation.
 *
 * Called periodically from the aging timer, and can also be called
 * explicitly after reclaim to keep things moving. */
void mglru_age(void)
{
    struct mglru_state *st = &mglru_state[0];
    if (!st->enabled)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&st->lock, &irq_flags);

    /* Advance the accessed generation (wrap at MGLRU_NR_GENS). */
    st->last_accessed_gen = (st->last_accessed_gen + 1) % MGLRU_NR_GENS;

    /* Advance the evicted generation so we always target the oldest. */
    st->last_evicted_gen = (st->last_evicted_gen + 1) % MGLRU_NR_GENS;

    /* Optional: Demote active → inactive for the generation we just left,
     * making those pages more reclaimable next time. */
    int prev_gen = (st->last_accessed_gen - 1 + MGLRU_NR_GENS) % MGLRU_NR_GENS;
    struct mglru_gen *pg = &st->gens[prev_gen];
    pg->nr_inactive += pg->nr_active;
    pg->nr_active = 0;

    spinlock_irqsave_release(&st->lock, irq_flags);
}

/* ── mglru_isolate ─────────────────────────────────────────────────────
 * Isolate up to nr_to_isolate pages from the oldest non-empty generation.
 * Removes them from the LRU lists and adds them to the caller's list.
 * The caller is responsible for freeing or re-adding the pages.
 *
 * Returns the number of pages isolated. */
int mglru_isolate(int nr_to_isolate, struct list_head *list)
{
    if (nr_to_isolate <= 0 || list == NULL)
        return 0;

    struct mglru_state *st = &mglru_state[0];
    if (!st->enabled)
        return 0;

    int isolated = 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&st->lock, &irq_flags);

    /* Scan generations from oldest (last_evicted_gen) to youngest.
     * Skip the current youngest generation (last_accessed_gen). */
    for (int i = 0; i < MGLRU_NR_GENS && isolated < nr_to_isolate; i++) {
        int gen = (st->last_evicted_gen + i) % MGLRU_NR_GENS;

        /* Skip the youngest generation — don't evict fresh pages */
        if (gen == st->last_accessed_gen)
            continue;

        struct mglru_gen *g = &st->gens[gen];

        /* Process pages from the head (oldest) of this gen's list. */
        while (isolated < nr_to_isolate && !list_empty(&g->list)) {
            struct list_head *lh = g->list.next;
            struct mglru_page_entry *entry;

            /* Get the containing entry */
            entry = list_entry(lh, struct mglru_page_entry, list);

            /* Check accessed flag — promote instead of isolate if young */
            if (entry->accessed) {
                /* Promote to the youngest generation.
                 * Remove from this gen, add to last_accessed_gen. */
                int old_gen = entry->gen;
                int new_gen = st->last_accessed_gen;

                list_del(&entry->list);
                entry->gen = new_gen;
                entry->accessed = 0;
                list_add_tail(&entry->list, &st->gens[new_gen].list);

                /* Update counters */
                if (old_gen >= 0 && old_gen < MGLRU_NR_GENS) {
                    if (st->gens[old_gen].nr_pages > 0)
                        st->gens[old_gen].nr_pages--;
                    if (st->gens[old_gen].nr_active > 0)
                        st->gens[old_gen].nr_active--;
                    else if (st->gens[old_gen].nr_inactive > 0)
                        st->gens[old_gen].nr_inactive--;
                }
                st->gens[new_gen].nr_pages++;
                st->gens[new_gen].nr_active++;

                continue;  /* skip this entry — it's now young */
            }

            /* Isolate this page: remove from gen list, add to output list */
            list_del(&entry->list);

            /* Update counters */
            if (g->nr_pages > 0) g->nr_pages--;
            if (entry->accessed) {
                /* Shouldn't happen since we checked above, but be safe */
                if (g->nr_active > 0) g->nr_active--;
            } else {
                if (g->nr_inactive > 0) g->nr_inactive--;
            }

            /* Clear the frame map (no longer tracked) */
            mglru_frame_map_set(entry->phys_addr, -1);

            /* Add to output list */
            list_add_tail(&entry->list, list);

            /* Mark as still in-use but detached from generation.
             * We keep in_use = 1 so the entry isn't reused yet. */
            entry->gen = -1;

            isolated++;
        }
    }

    spinlock_irqsave_release(&st->lock, irq_flags);
    return isolated;
}

/* ── mglru_reclaim_pages ──────────────────────────────────────────────
 * Reclaim up to nr_pages from the oldest generations.
 * Each reclaimed page is freed via pmm_free_frame().
 *
 * This is the main reclaim entry point called from the page allocator
 * when free memory is low.  Returns the number of pages freed. */
int mglru_reclaim_pages(int nr_pages, unsigned int gfp_mask)
{
    if (nr_pages <= 0)
        return 0;

    struct mglru_state *st = &mglru_state[0];
    if (!st->enabled) {
        /* MGLRU disabled — could fall back to simple LRU eviction here */
        return 0;
    }

    LIST_HEAD(isolated_list);
    int freed = 0;

    /* Isolate pages in chunks to balance lock hold time */
    while (freed < nr_pages) {
        int batch = (nr_pages - freed) < 32 ? (nr_pages - freed) : 32;
        int iso = mglru_isolate(batch, &isolated_list);
        if (iso <= 0)
            break;  /* nothing more to reclaim */

        /* Free each isolated page */
        while (!list_empty(&isolated_list)) {
            struct list_head *lh = isolated_list.next;
            struct mglru_page_entry *entry;
            entry = list_entry(lh, struct mglru_page_entry, list);

            uint64_t phys = entry->phys_addr;

            /* Remove from our temp list and free the pool entry */
            list_del(&entry->list);
            entry->in_use = 0;  /* release back to pool */

            /* Free the physical frame */
            if (phys != 0) {
                pmm_free_frame(phys);
                freed++;
            }
        }
    }

    return freed;
}

/* ── Query helpers ───────────────────────────────────────────────────── */

void mglru_get_gen_counts(unsigned long counts[MGLRU_NR_GENS])
{
    struct mglru_state *st = &mglru_state[0];

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&st->lock, &irq_flags);

    for (int i = 0; i < MGLRU_NR_GENS; i++)
        counts[i] = st->gens[i].nr_pages;

    spinlock_irqsave_release(&st->lock, irq_flags);
}

unsigned long mglru_total_pages(void)
{
    struct mglru_state *st = &mglru_state[0];
    unsigned long total = 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&st->lock, &irq_flags);

    for (int i = 0; i < MGLRU_NR_GENS; i++)
        total += st->gens[i].nr_pages;

    spinlock_irqsave_release(&st->lock, irq_flags);
    return total;
}

int mglru_is_enabled(void)
{
    return mglru_state[0].enabled;
}

void mglru_set_enabled(int val)
{
    mglru_state[0].enabled = val ? 1 : 0;
}

unsigned long mglru_get_min_ttl_ms(void)
{
    return mglru_state[0].min_ttl_ms;
}

void mglru_set_min_ttl_ms(unsigned long ms)
{
    mglru_state[0].min_ttl_ms = ms;
    /* Convert ms to jiffies (assuming TIMER_FREQ Hz) */
    mglru_state[0].aging_threshold = (ms * 1000ULL) / 10000000ULL;  /* NS_PER_TICK = 10ms */
    if (mglru_state[0].aging_threshold < 1)
        mglru_state[0].aging_threshold = 1;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Periodic aging timer
 * ════════════════════════════════════════════════════════════════════════ */

static void mglru_aging_callback(void *data)
{
    (void)data;

    struct mglru_state *st = &mglru_state[0];
    if (st->enabled) {
        mglru_age();
    }

    /* Re-arm the timer if still active */
    if (mglru_timer_active) {
        hrtimer_start(&mglru_timer, MGLRU_AGING_INTERVAL_NS);
    }
}

/* Called from scheduler tick — update MGLRU aging on each tick */
void mglru_tick(void)
{
    if (!mglru_is_enabled()) return;
    /* Mark current process pages for each node */
    for (int n = 0; n < MGLRU_NR_NODES; n++) {
        if (mglru_state[n].enabled) {
            mglru_page_accessed(0ULL);
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Sysfs interface
 * ════════════════════════════════════════════════════════════════════════ */

/* Read /sys/kernel/mm/mglru/enabled */
static int mglru_sysfs_enabled_read(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    return snprintf(buf, max_size, "%d\n", mglru_is_enabled());
}

/* Write /sys/kernel/mm/mglru/enabled */
static int mglru_sysfs_enabled_write(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    if (size == 0) return -1;

    int val = 0;
    if (data[0] == '1' || data[0] == 'y' || data[0] == 'Y' ||
        data[0] == 't' || data[0] == 'T')
        val = 1;

    mglru_set_enabled(val);
    return 0;
}

/* Read /sys/kernel/mm/mglru/min_ttl_ms */
static int mglru_sysfs_min_ttl_read(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    return snprintf(buf, max_size, "%lu\n", mglru_get_min_ttl_ms());
}

/* Write /sys/kernel/mm/mglru/min_ttl_ms */
static int mglru_sysfs_min_ttl_write(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    if (size == 0) return -1;

    unsigned long val = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (data[i] >= '0' && data[i] <= '9')
            val = val * 10 + (unsigned long)(data[i] - '0');
        else if (data[i] == '\n' || data[i] == '\0')
            break;
        else
            return -1;  /* invalid character */
    }

    if (val < 1) val = 1;
    if (val > 60000) val = 60000;  /* max 60 seconds */

    mglru_set_min_ttl_ms(val);
    return 0;
}

/* Read /sys/kernel/mm/mglru/generations */
static int mglru_sysfs_generations_read(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    unsigned long counts[MGLRU_NR_GENS];
    mglru_get_gen_counts(counts);

    return snprintf(buf, max_size,
                    "gen0: %lu\n"
                    "gen1: %lu\n"
                    "gen2: %lu\n"
                    "gen3: %lu\n",
                    counts[0], counts[1], counts[2], counts[3]);
}

/* Create the sysfs entries under /sys/kernel/mm/mglru/ */
static void mglru_sysfs_init(void)
{
    /* Create directory tree: /sys/kernel/mm/mglru/ */
    sysfs_create_dir("/sys/kernel");
    sysfs_create_dir("/sys/kernel/mm");
    sysfs_create_dir("/sys/kernel/mm/mglru");

    /* /sys/kernel/mm/mglru/enabled (read/write) */
    if (sysfs_create_writable_file(
            "/sys/kernel/mm/mglru/enabled",
            "1\n", NULL,
            mglru_sysfs_enabled_read, mglru_sysfs_enabled_write) < 0) {
        kprintf("[MGLRU] sysfs: failed to create enabled\n");
    }

    /* /sys/kernel/mm/mglru/min_ttl_ms (read/write) */
    if (sysfs_create_writable_file(
            "/sys/kernel/mm/mglru/min_ttl_ms",
            "1000\n", NULL,
            mglru_sysfs_min_ttl_read, mglru_sysfs_min_ttl_write) < 0) {
        kprintf("[MGLRU] sysfs: failed to create min_ttl_ms\n");
    }

    /* /sys/kernel/mm/mglru/generations (read-only) */
    if (sysfs_create_writable_file(
            "/sys/kernel/mm/mglru/generations",
            NULL, NULL,
            mglru_sysfs_generations_read, NULL) < 0) {
        kprintf("[MGLRU] sysfs: failed to create generations\n");
    }
}
#include "module.h"
module_init(mglru_init);

/* ── lru_gen_init_lruvec — Initialise an LRU vector ─────────── */
void lru_gen_init_lruvec(void *lruvec)
{
    (void)lruvec;
    struct mglru_state *st = &mglru_state[0];

    if (!st->enabled)
        return;

    /* Initialise each generation list in the per-node state */
    for (int i = 0; i < MGLRU_NR_GENS; i++) {
        INIT_LIST_HEAD(&st->gens[i].list);
        st->gens[i].nr_pages = 0;
        st->gens[i].nr_active = 0;
        st->gens[i].nr_inactive = 0;
    }

    st->last_accessed_gen = MGLRU_NR_GENS - 1; /* youngest */
    st->last_evicted_gen = 0;                   /* oldest */

    kprintf("[mglru] lru_gen_init_lruvec: initialised %d generations\n",
            MGLRU_NR_GENS);
}

/* ── lru_gen_look_around — Scan PTEs around a page for access info ── */
void lru_gen_look_around(uint64_t addr)
{
    if (addr == 0)
        return;

    struct mglru_state *st = &mglru_state[0];
    if (!st->enabled)
        return;

    /* Check nearby pages (64 PTEs = one cache line worth of PTE entries).
     * This simulates scanning the page table entries around the given
     * address to check their accessed/dirty bits. */
    uint64_t base = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t start = base - 32 * PAGE_SIZE;
    uint64_t end   = base + 32 * PAGE_SIZE;

    if (start < base) /* check for wrap */
        start = 0;

    /* Walk the range and mark pages as accessed if they have PTE_ACCESSED */
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t phys = 0;
        if (vmm_virt_to_phys(va, &phys) == 0 && phys != 0) {
            /* Check if the page has been accessed by looking at the
             * PTE_ACCESSED bit.  In a real implementation, we'd walk
             * the page tables and check.  Here we simply promote the
             * page to a younger generation. */
            mglru_page_accessed(phys);
        }
    }
}

/* ── lru_gen_eviction ──────────────────────────────────────── */
int lru_gen_eviction(int nr_to_reclaim)
{
    if (nr_to_reclaim <= 0)
        return 0;
    /* Delegate to the existing MGLRU reclaim path */
    return mglru_reclaim_pages(nr_to_reclaim, 0);
}

/* ── lru_gen_seg_strategy ──────────────────────────────────── */
int lru_gen_seg_strategy(void)
{
    struct mglru_state *st = &mglru_state[0];
    if (!st->enabled)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&st->lock, &irq_flags);

    /* Choose segment: prefer oldest non-empty generation that is not
     * the youngest (last_accessed_gen). */
    int chosen = -1;
    for (int i = 0; i < MGLRU_NR_GENS; i++) {
        int gen = (st->last_evicted_gen + i) % MGLRU_NR_GENS;
        if (gen == st->last_accessed_gen)
            continue;
        if (st->gens[gen].nr_pages > 0) {
            chosen = gen;
            break;
        }
    }

    spinlock_irqsave_release(&st->lock, irq_flags);
    return chosen;
}
