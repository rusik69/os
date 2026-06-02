#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include "types.h"

/* ── Page cache ────────────────────────────────────────────────── */

/* Maximum pages tracked in the page cache */
#define PAGE_CACHE_MAX_PAGES 1024

/* Page cache entry flags */
#define PAGE_CACHE_DIRTY      (1 << 0)

/* Initialize the page cache subsystem */
void page_cache_init(void);

/* Lookup a page in cache by inode + block index.
 * Returns a pointer to the cache entry, or NULL if not cached. */
struct page_cache_entry *page_cache_lookup(uint64_t ino, uint64_t block);

/* Add a data page to the cache.  'data' is PAGE_SIZE bytes.
 * Returns 0 on success, negative on error. */
int page_cache_add(uint64_t ino, uint64_t block, const void *data);

/* Remove a page from the cache (frees physical frame). */
void page_cache_remove(uint64_t ino, uint64_t block);

/* Mark a cached page as dirty (needs writeback). */
void page_cache_mark_dirty(uint64_t ino, uint64_t block);

/* Flush all dirty pages to backing store. */
void page_cache_flush(void);

/* Get a pointer to cached page data, or NULL if not present. */
void *page_cache_get_data(uint64_t ino, uint64_t block);

/* ── Readahead ──────────────────────────────────────────────────── */

/* Default readahead window size (in pages) */
#define READAHEAD_WINDOW_MIN     4
#define READAHEAD_WINDOW_MAX    32
#define READAHEAD_WINDOW_INIT    4

/* Maximum number of inodes tracked for readahead state */
#define READAHEAD_MAX_TRACKERS  32

/* Readahead state for a single inode */
struct readahead_state {
    uint64_t ino;               /* inode number (0 = free slot) */
    uint64_t last_block;        /* last block accessed */
    int      sequential;        /* sequential access counter (how many consecutive) */
    int      window;            /* current readahead window size */
    int      enabled;           /* 1 if readahead is active for this inode */
};

/* Read a block via the page cache with readahead.
 *
 * Instead of calling the backing_store function directly, use this
 * wrapper: it checks the cache, fetches from store on miss, and
 * automatically triggers readahead for subsequent blocks when a
 * sequential access pattern is detected.
 *
 * @ino          Inode number (file identifier)
 * @block        Block index within the file
 * @buf          Destination buffer (must be at least PAGE_SIZE bytes)
 * @backing_store Callback that reads one block from the underlying device.
 *                It receives (lba, count, buf) and must return 0 on success.
 *                Pass NULL to skip backing store (cache-only lookup).
 *
 * Returns 0 on success, negative on error.
 */
int page_cache_read(uint64_t ino, uint64_t block, void *buf,
                    int (*backing_store)(uint32_t lba, uint8_t count, void *buf));

/* Explicitly trigger readahead for a range of blocks.
 * Reads blocks [start, start + count) from the backing store into cache.
 * This is called automatically by page_cache_read on sequential access,
 * but can also be invoked explicitly by filesystem code.
 *
 * @ino          Inode number
 * @start_block  First block to prefetch
 * @count        Number of blocks to prefetch
 * @backing_store Callback for reading one block
 *
 * Returns the number of blocks successfully prefetched.
 */
int page_cache_readahead(uint64_t ino, uint64_t start_block, int count,
                         int (*backing_store)(uint32_t lba, uint8_t count, void *buf));

/* Reset readahead state for a given inode (e.g., on seek to non-adjacent offset). */
void page_cache_readahead_reset(uint64_t ino);

/* Print readahead statistics (for debugging / /proc). */
void page_cache_readahead_stats(int *hits, int *misses, int *prefetches);

/* ── Writeback / dirty page flushing ─────────────────────────────── */

/* Register backing-store write callback for dirty page writeback.
 * Called by the filesystem layer during init. */
void page_cache_set_writeback(int (*writeback)(uint32_t lba, uint8_t count, const void *buf));

/* Write data to the page cache and mark the page dirty.
 * The data is cached in memory; actual disk write happens when the
 * page is evicted or page_cache_flush() is called.
 * Returns 0 on success, negative on error. */
int page_cache_write(uint64_t ino, uint64_t block, const void *data);

/* Remove a page from the cache without writing it back.
 * Use when discarding data (e.g., truncate, unlink). */
void page_cache_discard(uint64_t ino, uint64_t block);

#endif /* PAGE_CACHE_H */
