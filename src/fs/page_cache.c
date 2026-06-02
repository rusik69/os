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
void page_cache_init(void) {
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


/* ── Flush all dirty pages ─────────────────────────────────────────── */
void page_cache_flush(void) {
    if (!writeback_fn) return;  /* no writeback registered — can't flush */

    int flushed = 0;
    spinlock_acquire(&dirty_lock);

    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        if (page_cache[i].in_use && (page_cache[i].flags & PAGE_CACHE_DIRTY)) {
            uint32_t lba = (uint32_t)(page_cache[i].block * (PAGE_SIZE / 512));
            uint8_t count = (uint8_t)(PAGE_SIZE / 512);
            if (writeback_fn(lba, count, page_cache[i].data) < 0) {
                kprintf("[page_cache] flush: writeback failed for ino=%llu block=%llu\n",
                        (unsigned long long)page_cache[i].ino,
                        (unsigned long long)page_cache[i].block);
                continue;  /* retry next time */
            }
            page_cache[i].flags &= ~PAGE_CACHE_DIRTY;
            flushed++;
        }
    }

    if (flushed > 0) {
        nr_dirty_pages -= flushed;
        if (nr_dirty_pages < 0) nr_dirty_pages = 0;
    }

    spinlock_release(&dirty_lock);

    if (flushed > 0) {
        kprintf("[page_cache] flush: %d dirty pages written back\n", flushed);
    }
}


/* ── Flush dirty pages for a specific inode ─────────────────────────── */

void page_cache_flush_inode(uint64_t ino) {
    if (!writeback_fn) return;  /* no writeback registered — can't flush */

    int flushed = 0;
    spinlock_acquire(&dirty_lock);

    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        if (page_cache[i].in_use && page_cache[i].ino == ino &&
            (page_cache[i].flags & PAGE_CACHE_DIRTY)) {
            uint32_t lba = (uint32_t)(page_cache[i].block * (PAGE_SIZE / 512));
            uint8_t count = (uint8_t)(PAGE_SIZE / 512);
            if (writeback_fn(lba, count, page_cache[i].data) < 0) {
                kprintf("[page_cache] flush_inode: writeback failed for ino=%llu block=%llu\n",
                        (unsigned long long)page_cache[i].ino,
                        (unsigned long long)page_cache[i].block);
                continue;  /* retry next time */
            }
            page_cache[i].flags &= ~PAGE_CACHE_DIRTY;
            flushed++;
        }
    }

    if (flushed > 0) {
        nr_dirty_pages -= flushed;
        if (nr_dirty_pages < 0) nr_dirty_pages = 0;
    }

    spinlock_release(&dirty_lock);

    if (flushed > 0) {
        kprintf("[page_cache] flush_inode: %d dirty pages written back for ino=%llu\n",
                flushed, (unsigned long long)ino);
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
    int flushed = 0;

    spinlock_acquire(&dirty_lock);

    for (int i = 0; i < PAGE_CACHE_MAX_PAGES && flushed < batch_size; i++) {
        if (page_cache[i].in_use && (page_cache[i].flags & PAGE_CACHE_DIRTY)) {
            uint32_t lba = (uint32_t)(page_cache[i].block * (PAGE_SIZE / 512));
            uint8_t count = (uint8_t)(PAGE_SIZE / 512);
            if (writeback_fn(lba, count, page_cache[i].data) == 0) {
                page_cache[i].flags &= ~PAGE_CACHE_DIRTY;
                flushed++;
            }
        }
    }

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
    int flushed = 0;

    spinlock_acquire(&dirty_lock);

    for (int i = 0; i < PAGE_CACHE_MAX_PAGES && flushed < batch; i++) {
        if (page_cache[i].in_use && (page_cache[i].flags & PAGE_CACHE_DIRTY)) {
            uint32_t lba = (uint32_t)(page_cache[i].block * (PAGE_SIZE / 512));
            uint8_t count = (uint8_t)(PAGE_SIZE / 512);
            if (writeback_fn(lba, count, page_cache[i].data) == 0) {
                page_cache[i].flags &= ~PAGE_CACHE_DIRTY;
                flushed++;
            }
        }
    }

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
