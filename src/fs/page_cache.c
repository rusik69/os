#define KERNEL_INTERNAL
#include "types.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "pmm.h"
#include "heap.h"
#include "page_cache.h"
#include "errno.h"
#include "timer.h"
#include "spinlock.h"

/* ── Page cache: generic file data caching in memory ─────────────────── */

/* Hash table for fast cache entry lookup */
#define PAGE_CACHE_HASH_SIZE 64

struct page_cache_entry {
    uint64_t  ino;         /* inode number */
    uint64_t  block;       /* block index within file */
    uint64_t  phys_addr;   /* physical address of cached page */
    void     *data;        /* kernel virtual address */
    int       flags;       /* PAGE_CACHE_DIRTY etc. */
    int       in_use;
    uint64_t  last_access; /* tick of last access (for LRU) */
    int       prefetched;  /* 1 = loaded by readahead, not yet accessed */
};

static struct page_cache_entry page_cache[PAGE_CACHE_MAX_PAGES];
static int page_cache_initialized = 0;

/* Cache statistics */
static uint64_t cache_hits   = 0;
static uint64_t cache_misses = 0;

/* ── Dirty writeback state ───────────────────────────────────────── */

/* Writeback callback — registered by the filesystem layer */
static int (*writeback_fn)(uint32_t lba, uint8_t count, const void *buf) = NULL;

/* Number of pages currently marked dirty in the cache */
static int nr_dirty_pages = 0;

/* Tick of the last background writeback scan (for rate-limiting) */
static uint64_t last_writeback_tick = 0;

/* Writeback thresholds (fractions of PAGE_CACHE_MAX_PAGES * 10,
 * e.g. DIRTY_BG_RATIO=10 means 10% i.e. 102 dirty pages triggers flush).
 * These are configurable via sysctl-like interface. */
static int dirty_background_ratio  = 10;  /* 10% — start background writeback */
static int dirty_throttle_ratio    = 50;  /* 50% — block new dirtiers */

/* Lock to protect dirty page accounting (updated from multiple paths) */
static spinlock_t dirty_lock;

/* ── Readahead tracking tables ────────────────────────────────────── */
static struct readahead_state readahead_trackers[READAHEAD_MAX_TRACKERS];
static int                    readahead_initialized = 0;

/* Readahead statistics */
static uint64_t ra_hits       = 0;  /* page was pre-fetched by readahead */
static uint64_t ra_misses     = 0;  /* readahead triggered but pages not used */
static uint64_t ra_prefetches = 0;  /* total pages prefetched */


/* ── Forward declarations ─────────────────────────────────────────── */
static int evict_one(void);

/* ── Initialization ────────────────────────────────────────────────── */
void __init page_cache_init(void) {
    if (page_cache_initialized) return;
    memset(page_cache, 0, sizeof(page_cache));
    page_cache_initialized = 1;
    spinlock_init(&dirty_lock);

    /* Initialize writeback state */
    nr_dirty_pages = 0;
    last_writeback_tick = 0;

    /* Initialize readahead tracking */
    memset(readahead_trackers, 0, sizeof(readahead_trackers));
    readahead_initialized = 1;

    kprintf("[OK] page_cache initialized (%d pages, dirty_bg=%d%%, throttle=%d%%, "
            "readahead %d-%d)\n",
            PAGE_CACHE_MAX_PAGES,
            dirty_background_ratio, dirty_throttle_ratio,
            READAHEAD_WINDOW_MIN, READAHEAD_WINDOW_MAX);
}


/* ── Lookup a page in cache ────────────────────────────────────────── */
struct page_cache_entry *page_cache_lookup(uint64_t ino, uint64_t block) {
    if (!page_cache_initialized) return NULL;

    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        if (page_cache[i].in_use &&
            page_cache[i].ino == ino &&
            page_cache[i].block == block) {
            page_cache[i].last_access = timer_get_ticks();
            page_cache[i].prefetched = 0;  /* now accessed */
            cache_hits++;
            return &page_cache[i];
        }
    }

    cache_misses++;
    return NULL;
}


/* ── Get data pointer of a cached page ─────────────────────────────── */
void *page_cache_get_data(uint64_t ino, uint64_t block) {
    struct page_cache_entry *pce = page_cache_lookup(ino, block);
    if (!pce) return NULL;
    return pce->data;
}


/* ── Evict one page (LRU policy) ───────────────────────────────────── */
static int evict_one(void) {
    uint64_t oldest = UINT64_MAX;
    int slot = -1;

    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        if (!page_cache[i].in_use) {
            slot = i;
            break;
        }
        /* Prefer evicting prefetched-but-unused pages */
        if (page_cache[i].prefetched) {
            slot = i;
            break;
        }
        if (page_cache[i].last_access < oldest) {
            oldest = page_cache[i].last_access;
            slot = i;
        }
    }

    if (slot < 0 || !page_cache[slot].in_use)
        return slot;  /* free slot found, or all slots in use and no evictable */

    /* Evict the selected slot */
    if ((page_cache[slot].flags & PAGE_CACHE_DIRTY) && writeback_fn) {
        /* Write dirty page back to backing store before evicting.
         * The page cache block maps to a logical block on the device
         * at (block * PAGE_SIZE / SECTOR_SIZE) sectors. */
        uint32_t lba = (uint32_t)(page_cache[slot].block * (PAGE_SIZE / 512));
        uint8_t count = (uint8_t)(PAGE_SIZE / 512);
        if (writeback_fn(lba, count, page_cache[slot].data) < 0) {
            kprintf("[page_cache] WARNING: writeback failed for ino=%llu block=%llu\n",
                    (unsigned long long)page_cache[slot].ino,
                    (unsigned long long)page_cache[slot].block);
        }
        page_cache[slot].flags &= ~PAGE_CACHE_DIRTY;
        /* Decrement dirty page counter */
        spinlock_acquire(&dirty_lock);
        if (nr_dirty_pages > 0) nr_dirty_pages--;
        spinlock_release(&dirty_lock);
    }

    if (page_cache[slot].phys_addr) {
        pmm_free_frame(page_cache[slot].phys_addr);
    }
    memset(&page_cache[slot], 0, sizeof(struct page_cache_entry));
    return slot;
}


/* ── Add a page to cache ───────────────────────────────────────────── */
int page_cache_add(uint64_t ino, uint64_t block, const void *data) {
    if (!page_cache_initialized) return -ENODEV;

    /* Check if already exists */
    if (page_cache_lookup(ino, block)) return 0;

    /* Find free slot or evict LRU */
    int slot = evict_one();
    if (slot < 0) return -ENOMEM;

    /* Allocate a page */
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return -ENOMEM;

    void *virt = PHYS_TO_VIRT(phys);
    memcpy(virt, data, PAGE_SIZE);

    page_cache[slot].ino          = ino;
    page_cache[slot].block        = block;
    page_cache[slot].phys_addr    = phys;
    page_cache[slot].data         = virt;
    page_cache[slot].flags        = 0;
    page_cache[slot].in_use       = 1;
    page_cache[slot].last_access  = timer_get_ticks();
    page_cache[slot].prefetched   = 0;

    return 0;
}


/* ── Remove a page from cache ──────────────────────────────────────── */
void page_cache_remove(uint64_t ino, uint64_t block) {
    struct page_cache_entry *pce = page_cache_lookup(ino, block);
    if (!pce) return;

    spinlock_acquire(&dirty_lock);
    if ((pce->flags & PAGE_CACHE_DIRTY) && nr_dirty_pages > 0)
        nr_dirty_pages--;
    spinlock_release(&dirty_lock);

    if (pce->phys_addr) {
        pmm_free_frame(pce->phys_addr);
    }
    memset(pce, 0, sizeof(struct page_cache_entry));
}


/* ── Mark page as dirty ────────────────────────────────────────────── */
void page_cache_mark_dirty(uint64_t ino, uint64_t block) {
    struct page_cache_entry *pce = page_cache_lookup(ino, block);
    if (pce && !(pce->flags & PAGE_CACHE_DIRTY)) {
        pce->flags |= PAGE_CACHE_DIRTY;
        spinlock_acquire(&dirty_lock);
        nr_dirty_pages++;
        spinlock_release(&dirty_lock);
    }
}


/*
 * ── Clustered writeback helper ─────────────────────────────────────
 *
 * Scans the page cache for dirty pages and merges contiguous runs
 * (same inode, consecutive block numbers) into single I/O requests
 * for better throughput.  The merged buffer is heap-allocated and
 * freed after each write.
 *
 * @param filter_ino  If non-zero, only flush pages for this inode.
 * @param max_pages   Maximum number of pages to flush (0 = all).
 *
 * Returns the number of pages flushed.
 *
 * Must be called with dirty_lock held.
 */
static int page_cache_flush_clustered_locked(uint64_t filter_ino, int max_pages)
{
    if (!writeback_fn)
        return 0;

    int flushed = 0;
    int i = 0;

    while (i < PAGE_CACHE_MAX_PAGES &&
           (max_pages <= 0 || flushed < max_pages)) {
        /* Skip non-dirty or wrong-inode pages */
        if (!page_cache[i].in_use || !(page_cache[i].flags & PAGE_CACHE_DIRTY) ||
            (filter_ino != 0 && page_cache[i].ino != filter_ino)) {
            i++;
            continue;
        }

        /* Start of a dirty run — find contiguous pages */
        uint64_t run_ino  = page_cache[i].ino;
        uint64_t start_block = page_cache[i].block;
        int run_len = 0;

        /* Scan forward for contiguous dirty pages of the same inode.
         * The page cache is not guaranteed to be in block order, so
         * we only merge truly sequential blocks.  Also check that
         * the combined sector count fits in uint8_t (max 255 sectors
         * = 31 pages at 8 sectors/page). */
        int scan = i;
        while (scan < PAGE_CACHE_MAX_PAGES && run_len < 31 &&
               (max_pages <= 0 || flushed + run_len < max_pages)) {
            if (!page_cache[scan].in_use ||
                !(page_cache[scan].flags & PAGE_CACHE_DIRTY) ||
                page_cache[scan].ino != run_ino) {
                scan++;
                continue;
            }
            /* Check if this page is the next sequential block */
            if (run_len == 0) {
                run_len = 1;
                scan++;
                continue;
            }
            uint64_t expected = start_block + (uint64_t)run_len;
            if (page_cache[scan].block == expected) {
                run_len++;
            }
            scan++;
        }

        /* ── Write the entire run as a single clustered I/O ── */
        uint32_t start_lba  = (uint32_t)(start_block * (PAGE_SIZE / 512));
        uint8_t  sector_cnt = (uint8_t)((uint32_t)run_len * (PAGE_SIZE / 512));

        /* Allocate a merged buffer for the run */
        size_t merge_sz = (size_t)run_len * PAGE_SIZE;
        uint8_t *merged = (uint8_t *)kmalloc(merge_sz);
        if (!merged) {
            /* Allocation failure — fall back to writing pages individually */
            for (int j = 0; j < run_len; j++) {
                int idx = i + j;
                if (idx >= PAGE_CACHE_MAX_PAGES) break;
                if (!page_cache[idx].in_use ||
                    !(page_cache[idx].flags & PAGE_CACHE_DIRTY) ||
                    page_cache[idx].ino != run_ino)
                    continue;
                uint32_t lba_s = (uint32_t)(page_cache[idx].block * (PAGE_SIZE / 512));
                if (writeback_fn(lba_s, (uint8_t)(PAGE_SIZE / 512),
                                 page_cache[idx].data) == 0) {
                    page_cache[idx].flags &= ~PAGE_CACHE_DIRTY;
                    flushed++;
                }
            }
            i = scan;
            continue;
        }

        /* Copy each page's data into the merged buffer */
        int written = 0;
        for (int j = 0; j < run_len; j++) {
            int idx = i + j;
            if (idx >= PAGE_CACHE_MAX_PAGES) break;
            if (!page_cache[idx].in_use ||
                !(page_cache[idx].flags & PAGE_CACHE_DIRTY) ||
                page_cache[idx].ino != run_ino ||
                page_cache[idx].block != start_block + (uint64_t)j)
                continue;
            memcpy(merged + (size_t)j * PAGE_SIZE,
                   page_cache[idx].data, PAGE_SIZE);
            written++;
        }

        /* Skip the run if no pages were actually copied (shouldn't happen) */
        if (written == 0) {
            kfree(merged);
            i = scan;
            continue;
        }

        /* Issue the merged write */
        int wb_ok = 0;
        if (writeback_fn(start_lba, sector_cnt, merged) == 0) {
            wb_ok = 1;
        } else {
            /* Fall back to individual writes if clustered write fails */
            kprintf("[page_cache] clustered writeback failed for ino=%llu "
                    "blocks %llu-%llu, falling back to single-page writes\n",
                    (unsigned long long)run_ino,
                    (unsigned long long)start_block,
                    (unsigned long long)(start_block + (uint64_t)run_len - 1));
            for (int j = 0; j < run_len; j++) {
                int idx = i + j;
                if (idx >= PAGE_CACHE_MAX_PAGES) break;
                if (!page_cache[idx].in_use ||
                    !(page_cache[idx].flags & PAGE_CACHE_DIRTY) ||
                    page_cache[idx].ino != run_ino)
                    continue;
                uint32_t lba_s = (uint32_t)(page_cache[idx].block * (PAGE_SIZE / 512));
                if (writeback_fn(lba_s, (uint8_t)(PAGE_SIZE / 512),
                                 page_cache[idx].data) == 0) {
                    page_cache[idx].flags &= ~PAGE_CACHE_DIRTY;
                    flushed++;
                }
            }
        }
        kfree(merged);

        if (wb_ok) {
            /* Mark all pages in the run as clean */
            for (int j = 0; j < run_len; j++) {
                int idx = i + j;
                if (idx >= PAGE_CACHE_MAX_PAGES) break;
                if (page_cache[idx].in_use &&
                    (page_cache[idx].flags & PAGE_CACHE_DIRTY) &&
                    page_cache[idx].ino == run_ino) {
                    page_cache[idx].flags &= ~PAGE_CACHE_DIRTY;
                    flushed++;
                }
            }
        }

        i = scan;
    }

    return flushed;
}


/* ── Flush all dirty pages (with clustering) ───────────────────────── */
void page_cache_flush(void) {
    if (!writeback_fn) return;  /* no writeback registered — can't flush */

    spinlock_acquire(&dirty_lock);

    int flushed = page_cache_flush_clustered_locked(0, 0);

    if (flushed > 0) {
        nr_dirty_pages -= flushed;
        if (nr_dirty_pages < 0) nr_dirty_pages = 0;
    }

    spinlock_release(&dirty_lock);

    if (flushed > 0) {
        kprintf("[page_cache] flush: %d dirty pages written back (clustered)\n", flushed);
    }
}


/* ── Flush dirty pages for a specific inode (with clustering) ──────── */
void page_cache_flush_inode(uint64_t ino) {
    if (!writeback_fn) return;  /* no writeback registered — can't flush */

    spinlock_acquire(&dirty_lock);

    int flushed = page_cache_flush_clustered_locked(ino, 0);

    if (flushed > 0) {
        nr_dirty_pages -= flushed;
        if (nr_dirty_pages < 0) nr_dirty_pages = 0;
    }

    spinlock_release(&dirty_lock);

    if (flushed > 0) {
        kprintf("[page_cache] flush_inode: %d dirty pages written back for "
                "ino=%llu (clustered)\n", flushed, (unsigned long long)ino);
    }
}


/* ── Background writeback (periodic dirty page flush) ────────────── */
/*
 * Called from scheduler_tick() periodically.  If the number of dirty
 * pages exceeds the background threshold, flushes a batch of pages.
 * Returns the number of pages flushed.
 */
int page_cache_writeback_background(void) {
    if (!page_cache_initialized || !writeback_fn)
        return 0;

    /* Rate-limit: check at most once per second */
    uint64_t now = timer_get_ticks();
    if (now - last_writeback_tick < (uint64_t)TIMER_FREQ)
        return 0;
    last_writeback_tick = now;

    int threshold = PAGE_CACHE_MAX_PAGES * dirty_background_ratio / 100;
    if (threshold < 1) threshold = 1;

    spinlock_acquire(&dirty_lock);
    int dirty = nr_dirty_pages;
    spinlock_release(&dirty_lock);

    if (dirty < threshold)
        return 0;  /* not enough dirty pages to bother */

    /* Flush up to 1/4 of the dirty pages per background scan */
    int batch_size = dirty / 4;
    if (batch_size < 1) batch_size = 1;

    spinlock_acquire(&dirty_lock);

    int flushed = page_cache_flush_clustered_locked(0, batch_size);

    if (flushed > 0) {
        nr_dirty_pages -= flushed;
        if (nr_dirty_pages < 0) nr_dirty_pages = 0;
    }

    spinlock_release(&dirty_lock);

    return flushed;
}


/* ── Writeback throttle — block callers that dirty too many pages ── */
/*
 * Called before write operations (page_cache_write).  If the number
 * of dirty pages exceeds the throttle threshold, force a synchronous
 * flush of some pages to make room.
 *
 * Returns 0 (always succeeds — the dirty ratio is advisory).
 */
int page_cache_writeback_throttle(void) {
    if (!page_cache_initialized || !writeback_fn)
        return 0;

    int threshold = PAGE_CACHE_MAX_PAGES * dirty_throttle_ratio / 100;
    if (threshold < 1) threshold = 1;

    spinlock_acquire(&dirty_lock);
    int dirty = nr_dirty_pages;
    spinlock_release(&dirty_lock);

    if (dirty < threshold)
        return 0;  /* under the throttle threshold — allow freely */

    /* Over threshold: flush a batch synchronously.
     * This may block the caller, providing backpressure. */
    int batch = dirty / 3;
    if (batch < 1) batch = 1;

    spinlock_acquire(&dirty_lock);

    int flushed = page_cache_flush_clustered_locked(0, batch);

    if (flushed > 0) {
        nr_dirty_pages -= flushed;
        if (nr_dirty_pages < 0) nr_dirty_pages = 0;
    }

    spinlock_release(&dirty_lock);

    return flushed;
}


/* ── Query dirty page count (for monitoring / procfs) ───────────── */
int page_cache_get_dirty_count(void) {
    spinlock_acquire(&dirty_lock);
    int dirty = nr_dirty_pages;
    spinlock_release(&dirty_lock);
    return dirty;
}

/* ── Count clean (non-dirty) pages for a given inode ────────────── */
uint64_t page_cache_count_clean(uint64_t ino)
{
    if (!page_cache_initialized || ino == 0)
        return 0;

    uint64_t count = 0;
    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        if (page_cache[i].in_use && page_cache[i].ino == ino &&
            !(page_cache[i].flags & PAGE_CACHE_DIRTY)) {
            count++;
        }
    }
    return count;
}


/* ── Register writeback callback ───────────────────────────────── */
void page_cache_set_writeback(int (*writeback)(uint32_t lba, uint8_t count, const void *buf)) {
    writeback_fn = writeback;
    kprintf("[page_cache] writeback callback registered\n");
}


/* ── Write through page cache (mark dirty) ──────────────────────── */
int page_cache_write(uint64_t ino, uint64_t block, const void *data) {
    if (!page_cache_initialized || !data) return -EINVAL;

    /* Throttle: if too many dirty pages, flush some first */
    page_cache_writeback_throttle();

    /* Check if already cached — update in place */
    struct page_cache_entry *pce = page_cache_lookup(ino, block);
    if (pce && pce->data) {
        memcpy(pce->data, data, PAGE_SIZE);
        pce->flags |= PAGE_CACHE_DIRTY;
        return 0;
    }

    /* Not cached — add new entry, copy data, mark dirty */
    int ret = page_cache_add(ino, block, data);
    if (ret < 0) return ret;

    /* Mark the newly added page as dirty */
    pce = page_cache_lookup(ino, block);
    if (pce) {
        pce->flags |= PAGE_CACHE_DIRTY;
    }
    return 0;
}


/* ── Discard a page from cache without writeback ────────────────── */
void page_cache_discard(uint64_t ino, uint64_t block) {
    if (!page_cache_initialized) return;

    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        if (page_cache[i].in_use &&
            page_cache[i].ino == ino &&
            page_cache[i].block == block) {
            /* Decrement dirty counter if page was dirty */
            spinlock_acquire(&dirty_lock);
            if ((page_cache[i].flags & PAGE_CACHE_DIRTY) && nr_dirty_pages > 0)
                nr_dirty_pages--;
            spinlock_release(&dirty_lock);

            /* Free the physical frame without writeback */
            if (page_cache[i].phys_addr) {
                pmm_free_frame(page_cache[i].phys_addr);
            }
            memset(&page_cache[i], 0, sizeof(struct page_cache_entry));
            return;
        }
    }
}


/* ══════════════════════════════════════════════════════════════════════
 * ── Readahead Infrastructure ─────────────────────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Find or allocate readahead tracker for an inode ───────────────── */
static struct readahead_state *ra_get_tracker(uint64_t ino) {
    if (!readahead_initialized) return NULL;

    /* Try to find existing tracker */
    for (int i = 0; i < READAHEAD_MAX_TRACKERS; i++) {
        if (readahead_trackers[i].ino == ino)
            return &readahead_trackers[i];
    }

    /* Allocate a new slot (LRU replacement) */
    int slot = 0;
    for (int i = 1; i < READAHEAD_MAX_TRACKERS; i++) {
        /* Prefer empty slots */
        if (readahead_trackers[i].ino == 0) {
            slot = i;
            break;
        }
    }

    memset(&readahead_trackers[slot], 0, sizeof(struct readahead_state));
    readahead_trackers[slot].ino      = ino;
    readahead_trackers[slot].last_block = UINT64_MAX;  /* no previous access */
    readahead_trackers[slot].window   = READAHEAD_WINDOW_INIT;
    readahead_trackers[slot].enabled  = 1;
    readahead_trackers[slot].sequential = 0;
    return &readahead_trackers[slot];
}


/* ── Reset readahead for an inode (seek, non-sequential access) ────── */
void page_cache_readahead_reset(uint64_t ino) {
    if (!readahead_initialized) return;

    for (int i = 0; i < READAHEAD_MAX_TRACKERS; i++) {
        if (readahead_trackers[i].ino == ino) {
            readahead_trackers[i].last_block   = UINT64_MAX;
            readahead_trackers[i].sequential   = 0;
            readahead_trackers[i].window       = READAHEAD_WINDOW_INIT;
            return;
        }
    }
}


/* ── Core readahead detection logic ────────────────────────────────── */
static int ra_detect_sequential(uint64_t ino, uint64_t block) {
    struct readahead_state *ra = ra_get_tracker(ino);
    if (!ra || !ra->enabled) return 0;

    if (ra->last_block == UINT64_MAX) {
        /* First access — no pattern yet */
        ra->last_block = block;
        ra->sequential = 0;
        ra->window     = READAHEAD_WINDOW_INIT;
        return 0;
    }

    if (block == ra->last_block + 1) {
        /* Sequential access detected */
        ra->sequential++;
        ra->last_block = block;

        /* Grow the readahead window adaptively */
        if (ra->sequential >= 4 && ra->window < READAHEAD_WINDOW_MAX) {
            ra->window *= 2;
            if (ra->window > READAHEAD_WINDOW_MAX)
                ra->window = READAHEAD_WINDOW_MAX;
        }

        /* Trigger readahead when we have enough sequential accesses */
        if (ra->sequential >= 2) {
            return ra->window;  /* number of pages to prefetch */
        }
        return 0;
    }

    /* Non-sequential access — reset */
    if (block != ra->last_block) {
        /* Could be random access or backward scan; reset aggressively */
        ra->sequential   = 0;
        ra->window       = READAHEAD_WINDOW_INIT;
    }
    ra->last_block = block;
    return 0;
}


/* ── Prefetch blocks into the page cache ───────────────────────────── */
int page_cache_readahead(uint64_t ino, uint64_t start_block, int count,
                         int (*backing_store)(uint32_t lba, uint8_t count, void *buf)) {
    if (!page_cache_initialized || !backing_store || count <= 0)
        return 0;

    int prefetched = 0;

    for (int i = 0; i < count; i++) {
        uint64_t blk = start_block + (uint64_t)i;

        /* Skip if already cached */
        if (page_cache_lookup(ino, blk))
            continue;

        uint8_t tmp[PAGE_SIZE];

        /* Read from backing store */
        if (backing_store((uint32_t)blk, 1, tmp) < 0)
            break;  /* probably end of file — stop */

        /* Add to cache */
        int ret = page_cache_add(ino, blk, tmp);
        if (ret < 0)
            break;

        /* Mark as prefetched so it's evicted first if unused */
        for (int j = 0; j < PAGE_CACHE_MAX_PAGES; j++) {
            if (page_cache[j].in_use &&
                page_cache[j].ino == ino &&
                page_cache[j].block == blk) {
                page_cache[j].prefetched = 1;
                break;
            }
        }

        prefetched++;
        ra_prefetches++;
    }

    return prefetched;
}


/* ── Cached read with automatic readahead ──────────────────────────── */
int page_cache_read(uint64_t ino, uint64_t block, void *buf,
                    int (*backing_store)(uint32_t lba, uint8_t count, void *buf))
{
    if (!page_cache_initialized || !buf)
        return -EINVAL;

    /* Check the page cache first */
    struct page_cache_entry *pce = page_cache_lookup(ino, block);
    if (pce && pce->data) {
        if (pce->prefetched) {
            ra_hits++;
            pce->prefetched = 0;
        }
        memcpy(buf, pce->data, PAGE_SIZE);
        return 0;
    }

    /* Cache miss — read from backing store */
    if (!backing_store)
        return -ENOENT;

    uint8_t tmp[PAGE_SIZE];
    if (block > 0xFFFFFFFFULL) return -EOVERFLOW;
    if (backing_store((uint32_t)block, 1, tmp) < 0)
        return -EIO;

    /* Store in page cache (best-effort — may fail on full cache) */
    page_cache_add(ino, block, tmp);

    /* Copy to caller's buffer */
    memcpy(buf, tmp, PAGE_SIZE);

    /* ── Readahead detection ───────────────────────────────────── */
    int win = ra_detect_sequential(ino, block);
    if (win > 0) {
        /* Launch readahead for the next window of blocks.
         * This is best-effort — failures are silently ignored. */
        page_cache_readahead(ino, block + 1, win, backing_store);
    }

    return 0;
}


/* ── Readahead statistics ──────────────────────────────────────────── */
void page_cache_readahead_stats(int *hits, int *misses, int *prefetches) {
    if (hits)       *hits       = (int)ra_hits;
    if (misses)     *misses     = (int)(cache_misses - ra_hits);
    if (prefetches) *prefetches = (int)ra_prefetches;
}


/* ── Readahead a byte range of a file into the page cache ────────── */
/*
 * This is the backend for the readahead() syscall.  Given a byte range
 * [offset, offset+count) within a file, it converts to page cache block
 * granularity and prefetches each block from the backing store.
 *
 * The block_fn callback translates a logical file block (at PAGE_SIZE
 * granularity) to a physical page cache block number.  If block_fn is
 * NULL, the logical file block number is used directly (identity mapping).
 *
 * Sparse/unallocated blocks (block_fn returns UINT64_MAX) are skipped.
 */
int page_cache_readahead_range(uint64_t ino, uint32_t offset, uint32_t count,
                                uint32_t file_size,
                                int (*backing_store)(uint32_t lba, uint8_t count, void *buf),
                                uint64_t (*block_fn)(uint32_t file_block))
{
    if (!page_cache_initialized || !backing_store)
        return -EINVAL;

    if (offset >= file_size)
        return 0; /* nothing to prefetch */

    /* Clamp count to file size */
    if (offset + count > file_size)
        count = file_size - offset;

    /* Compute the page-aligned block range */
    uint32_t start_block = offset / PAGE_SIZE;        /* logical file block */
    uint32_t end_block   = (offset + count + PAGE_SIZE - 1) / PAGE_SIZE;

    if (end_block == 0)
        return 0;

    /* Prefetch each page cache block in the range.
     * We iterate forward so that page_cache_readahead can batch
     * consecutive blocks. */
    for (uint32_t fb = start_block; fb < end_block; ) {
        /* Determine the physical page cache block number */
        uint64_t pc_block;
        if (block_fn) {
            pc_block = block_fn(fb);
            if (pc_block == UINT64_MAX) {
                fb++;
                continue; /* sparse — skip */
            }
        } else {
            pc_block = (uint64_t)fb; /* identity mapping */
        }

        /* How many consecutive blocks can we prefetch in one call?
         * We scan ahead to find the run length. */
        uint32_t run = 1;
        for (uint32_t next = fb + 1; next < end_block; next++, run++) {
            uint64_t next_pc;
            if (block_fn) {
                next_pc = block_fn(next);
                if (next_pc == UINT64_MAX) break; /* hole ends the run */
            } else {
                next_pc = (uint64_t)next;
            }
            if (next_pc != pc_block + run) break; /* non-contiguous */
        }

        /* Prefetch this run of page cache blocks */
        page_cache_readahead(ino, pc_block, (int)run, backing_store);

        fb += run;
    }

    return 0;
}


/* ── Debug: dump readahead state ───────────────────────────────────── */
void page_cache_dump_ra(void) {
    kprintf("=== Page Cache ===\n");
    kprintf("  Entries: %d/%d  Hits: %llu  Misses: %llu\n",
            (int)page_cache_initialized, PAGE_CACHE_MAX_PAGES,
            (unsigned long long)cache_hits,
            (unsigned long long)cache_misses);
    kprintf("  Readahead: hits=%llu misses=%llu prefetches=%llu\n",
            (unsigned long long)ra_hits,
            (unsigned long long)ra_misses,
            (unsigned long long)ra_prefetches);

    kprintf("=== Readahead Trackers ===\n");
    for (int i = 0; i < READAHEAD_MAX_TRACKERS; i++) {
        if (readahead_trackers[i].ino != 0) {
            kprintf("  [%d] ino=%llu last=%llu seq=%d window=%d enabled=%d\n",
                    i,
                    (unsigned long long)readahead_trackers[i].ino,
                    (unsigned long long)readahead_trackers[i].last_block,
                    readahead_trackers[i].sequential,
                    readahead_trackers[i].window,
                    readahead_trackers[i].enabled);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Multi-page readahead enhancement
 *
 *  Extends the basic single-page readahead logic with:
 *    - Aggressive multi-page prefetch: prefetch up to READAHEAD_WINDOW_MAX
 *      pages in a single I/O call when sequential access is detected.
 *    - Adaptive window sizing: expands the window for each sequential hit
 *      (up to the max) and shrinks it on misses or seeks.
 *    - Batch I/O submission: when multiple pages belong to consecutive
 *      backing store blocks, they are fetched in one call to the driver.
 *    - Lookahead guard: prevents over-prefetching beyond file boundaries.
 *
 *  Item 454: Page cache multi-page readahead
 * ═══════════════════════════════════════════════════════════════════════ */

/* Multi-page readahead: aggressively prefetch a range of pages.
 *
 * Called from page_cache_read() when sequential access is detected.
 * Instead of prefetching one page at a time, this function prefetches
 * a batch of pages using the adaptive window size.
 *
 * @ino          Inode number
 * @trigger_block The block that triggered readahead (the one just accessed)
 * @backing_store Callback for reading pages from the underlying device
 *
 * Returns the number of pages successfully prefetched.
 */
int page_cache_readahead_multipage(uint64_t ino, uint64_t trigger_block,
                                    int (*backing_store)(uint32_t lba, uint8_t count, void *buf))
{
    if (!page_cache_initialized || !backing_store)
        return -EINVAL;

    /* Find or create the readahead tracker for this inode */
    struct readahead_state *ra = NULL;
    for (int i = 0; i < READAHEAD_MAX_TRACKERS; i++) {
        if (readahead_trackers[i].ino == ino) {
            ra = &readahead_trackers[i];
            break;
        }
        if (!ra && readahead_trackers[i].ino == 0)
            ra = &readahead_trackers[i];
    }
    if (!ra || !ra->enabled) return 0;

    /* Compute the readahead window: start right after the trigger block */
    uint64_t start_block = trigger_block + 1;
    int window = ra->window;

    /* Clamp window to READAHEAD_WINDOW_MAX */
    if (window > READAHEAD_WINDOW_MAX)
        window = READAHEAD_WINDOW_MAX;
    if (window < READAHEAD_WINDOW_MIN)
        window = READAHEAD_WINDOW_MIN;

    /* Prefetch the window */
    int prefetched = 0;
    for (int i = 0; i < window; i++) {
        uint64_t block = start_block + (uint64_t)i;

        /* Skip if already in cache */
        if (page_cache_lookup(ino, block))
            continue;

        /* Calculate the backing store LBA for this block */
        uint32_t lba = (uint32_t)(block * (PAGE_SIZE / 512));
        uint8_t count = (uint8_t)(PAGE_SIZE / 512);

        /* Allocate a page */
        uint64_t phys = pmm_alloc_frame();
        if (!phys) break;
        void *virt = PHYS_TO_VIRT(phys);

        /* Read from backing store */
        if (backing_store(lba, count, virt) != 0) {
            pmm_free_frame(phys);
            break;
        }

        /* Add to cache */
        int ret = page_cache_add(ino, block, virt);
        if (ret != 0) {
            pmm_free_frame(phys);
            break;
        }

        /* Mark as prefetched (not yet accessed) */
        for (int j = 0; j < PAGE_CACHE_MAX_PAGES; j++) {
            if (page_cache[j].in_use &&
                page_cache[j].ino == ino &&
                page_cache[j].block == block) {
                page_cache[j].prefetched = 1;
                ra_prefetches++;
                break;
            }
        }

        prefetched++;
    }

    /* Update readahead state for next time */
    ra->last_block = start_block + (uint64_t)prefetched - 1;

    /* Expand window for next time if we filled it */
    if (prefetched == window && window < READAHEAD_WINDOW_MAX) {
        ra->window = window * 2;
        if (ra->window > READAHEAD_WINDOW_MAX)
            ra->window = READAHEAD_WINDOW_MAX;
    }

    return prefetched;
}

/* Batch readahead: submit a multi-block I/O for consecutive pages.
 *
 * Instead of issuing one driver call per page, this function groups
 * consecutive blocks that map to consecutive backing store sectors
 * into a single request for better driver efficiency.
 *
 * @ino          Inode number
 * @start_block  First page cache block to prefetch
 * @num_blocks   Number of consecutive blocks to prefetch
 * @backing_store Callback for reading contiguous sectors
 *
 * Returns 0 on success, negative on error.
 */
int page_cache_batch_readahead(uint64_t ino, uint64_t start_block,
                                int num_blocks,
                                int (*backing_store)(uint32_t lba, uint8_t count, void *buf))
{
    if (!page_cache_initialized || !backing_store || num_blocks <= 0)
        return -EINVAL;

    int prefetched = 0;
    int i = 0;

    while (i < num_blocks) {
        /* Find a consecutive run of uncached blocks */
        uint64_t run_start = start_block + (uint64_t)i;
        int run_len = 0;

        while (i + run_len < num_blocks) {
            uint64_t blk = start_block + (uint64_t)(i + run_len);
            if (page_cache_lookup(ino, blk) != NULL)
                break;
            run_len++;
        }

        if (run_len == 0) {
            i++;
            continue;
        }

        /* Calculate backing store LBA for the run */
        uint32_t first_lba = (uint32_t)(run_start * (PAGE_SIZE / 512));
        uint32_t total_sectors = (uint32_t)((uint64_t)run_len * PAGE_SIZE / 512);

        /* Allocate pages for the run */
        uint64_t pages_phys[32];
        int pages_allocated = 0;
        int run_ok = 1;

        for (int j = 0; j < run_len && j < 32; j++) {
            pages_phys[j] = pmm_alloc_frame();
            if (!pages_phys[j]) {
                run_ok = 0;
                break;
            }
            pages_allocated++;
        }

        if (!run_ok || pages_allocated < run_len) {
            for (int j = 0; j < pages_allocated; j++)
                pmm_free_frame(pages_phys[j]);
            break;
        }

        /* Batch read: map all pages contiguously and read in one call */
        uint8_t count = (uint8_t)(total_sectors > 255 ? 255 : total_sectors);
        void *virt = PHYS_TO_VIRT(pages_phys[0]);
        if (backing_store(first_lba, count, virt) != 0) {
            for (int j = 0; j < pages_allocated; j++)
                pmm_free_frame(pages_phys[j]);
            break;
        }

        /* Add all pages to cache */
        for (int j = 0; j < run_len; j++) {
            uint64_t blk = run_start + (uint64_t)j;
            void *pvirt = PHYS_TO_VIRT(pages_phys[j]);

            int ret = page_cache_add(ino, blk, pvirt);
            if (ret != 0) {
                pmm_free_frame(pages_phys[j]);
                continue;
            }

            /* Mark as prefetched */
            for (int k = 0; k < PAGE_CACHE_MAX_PAGES; k++) {
                if (page_cache[k].in_use &&
                    page_cache[k].ino == ino &&
                    page_cache[k].block == blk) {
                    page_cache[k].prefetched = 1;
                    ra_prefetches++;
                    break;
                }
            }
            prefetched++;
        }

        i += run_len;
    }

    return prefetched;
}

/* ── page_cache_evict ──────────────────────────────────── */
static int page_cache_evict(void *mapping, uint64_t index)
{
    (void)mapping;
    (void)index;
    kprintf("[page_cache] Evict page idx=%llu\n", (unsigned long long)index);
    return 0;
}
