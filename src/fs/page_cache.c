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

/* Writeback callback — registered by the filesystem layer */
static int (*writeback_fn)(uint32_t lba, uint8_t count, const void *buf) = NULL;

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

    /* Initialize readahead tracking */
    memset(readahead_trackers, 0, sizeof(readahead_trackers));
    readahead_initialized = 1;

    kprintf("[OK] page_cache initialized (%d pages, readahead window %d-%d)\n",
            PAGE_CACHE_MAX_PAGES, READAHEAD_WINDOW_MIN, READAHEAD_WINDOW_MAX);
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

    if (pce->phys_addr) {
        pmm_free_frame(pce->phys_addr);
    }
    memset(pce, 0, sizeof(struct page_cache_entry));
}


/* ── Mark page as dirty ────────────────────────────────────────────── */
void page_cache_mark_dirty(uint64_t ino, uint64_t block) {
    struct page_cache_entry *pce = page_cache_lookup(ino, block);
    if (pce) pce->flags |= PAGE_CACHE_DIRTY;
}


/* ── Flush all dirty pages ─────────────────────────────────────────── */
void page_cache_flush(void) {
    if (!writeback_fn) return;  /* no writeback registered — can't flush */

    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        if (page_cache[i].in_use && (page_cache[i].flags & PAGE_CACHE_DIRTY)) {
            uint32_t lba = (uint32_t)(page_cache[i].block * (PAGE_SIZE / 512));
            uint8_t count = (uint8_t)(PAGE_SIZE / 512);
            if (writeback_fn(lba, count, page_cache[i].data) < 0) {
                kprintf("[page_cache] flush: writeback failed for ino=%llu block=%llu\n",
                        (unsigned long long)page_cache[i].ino,
                        (unsigned long long)page_cache[i].block);
            }
            page_cache[i].flags &= ~PAGE_CACHE_DIRTY;
        }
    }
}

/* ── Register writeback callback ───────────────────────────────── */
void page_cache_set_writeback(int (*writeback)(uint32_t lba, uint8_t count, const void *buf)) {
    writeback_fn = writeback;
    kprintf("[page_cache] writeback callback registered\n");
}


/* ── Write through page cache (mark dirty) ──────────────────────── */
int page_cache_write(uint64_t ino, uint64_t block, const void *data) {
    if (!page_cache_initialized || !data) return -EINVAL;

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
