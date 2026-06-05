#ifndef DCACHE_H
#define DCACHE_H

#include "types.h"

/*
 * dcache.h — VFS dentry (path resolution) cache
 *
 * Provides a fixed-size LRU cache mapping paths to their resolved metadata
 * (type, size, permissions, etc.).  Speeds up repeated stat/lookup calls and
 * includes a shrink mechanism for reclaiming entries under memory pressure.
 *
 * Each entry holds the same data that vfs_stat() returns, keyed by absolute
 * path.  The cache is entirely optional — if it misses, the caller falls
 * through to the real filesystem ops.
 */

/* Maximum number of cached dentries */
#define DCACHE_SIZE     128

/* Maximum path length stored in cache (must match VFS abs_path buffer) */
#define DCACHE_PATH_LEN 128

/*
 * A single dentry cache entry.
 * Fields mirror struct vfs_stat plus the mount pointer for invalidation.
 */
struct dcache_entry {
    char  path[DCACHE_PATH_LEN];   /* absolute path (the lookup key) */
    void *mount;                    /* opaque mount cookie for bulk invalidate */

    uint8_t  type;            /* 1=file, 2=dir, 3=link */
    uint32_t size;
    uint16_t uid;
    uint16_t gid;
    uint16_t mode;
    uint32_t mtime;
    uint32_t atime;
    uint32_t nlink;
    uint32_t ino;             /* inode number */
    uint16_t dev_major;       /* device major for device nodes */
    uint16_t dev_minor;       /* device minor for device nodes */

    uint32_t last_tick;       /* system tick at last access (for LRU) */
    int      in_use;          /* 1 = slot occupied */
};

/* ── Initialisation ─────────────────────────────────────────────── */

/* Zero the entire cache.  Called once during VFS init. */
void dcache_init(void);

/* ── Lookup / insert / remove ───────────────────────────────────── */

/* Look up a path in the cache.  Returns pointer to entry or NULL. */
struct dcache_entry *dcache_lookup(const char *path);

/* Insert (or update) a cache entry for the given path + metadata.
 * If the cache is full, the LRU entry is evicted first. */
void dcache_add(const char *path, void *mount,
                uint8_t type, uint32_t size,
                uint16_t uid, uint16_t gid, uint16_t mode,
                uint32_t mtime, uint32_t atime, uint32_t nlink,
                uint32_t ino, uint16_t dev_major, uint16_t dev_minor);

/* Remove a single entry by path (called when a file is deleted). */
void dcache_remove(const char *path);

/* Invalidate all entries belonging to a given mount (called on umount). */
void dcache_remove_mount(void *mount);

/* ── Cache shrink (memory-pressure callback) ──────────────────────── */

/*
 * Evict up to @target_count entries, starting from the least recently used.
 * Returns the number actually evicted.
 */
int dcache_shrink(int target_count);

/* Evict the single least-recently-used entry.  Returns 1 if evicted, 0 if empty. */
int dcache_evict_one(void);

/* Return the current number of populated entries. */
int dcache_fill_count(void);

/* Return the total capacity (DCACHE_SIZE). */
int dcache_capacity(void);

#endif /* DCACHE_H */
