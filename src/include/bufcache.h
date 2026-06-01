#ifndef BUFCACHE_H
#define BUFCACHE_H

#include "types.h"

#define SECT_SIZE 512

/* ── Initialization ─────────────────────────────────────────────────── */
void bufcache_init(void);
void bufcache_enable(void);
void bufcache_disable(void);

/* ─── Core I/O ──────────────────────────────────────────────────────── *
 * bufcache_read returns a pointer to cached sector data (read-only
 * after release), or NULL on error (caller should fall back to direct I/O).
 *
 * After modifying data via bufcache_read pointer, call bufcache_mark_dirty
 * to schedule write-back.
 *
 * bufcache_write copies data into cache and marks it dirty.
 */
void *bufcache_read(uint64_t lba, uint8_t dev_id);
int   bufcache_mark_dirty(uint64_t lba, uint8_t dev_id);
int   bufcache_write(uint64_t lba, uint8_t dev_id, const void *data);

/* ── Flush / Invalidate ─────────────────────────────────────────────── */
void bufcache_flush(void);         /* write back all dirty entries */
void bufcache_flush_all(void);     /* alias */
void bufcache_invalidate(uint64_t lba, uint8_t dev_id);

/* Writeback: flush dirty pages from cache */
int bufcache_writeback(void);
void bufcache_set_dirty(uint64_t lba, uint8_t dev_id);
void bufcache_clear_dirty(uint64_t lba, uint8_t dev_id);

/* ── Stats ──────────────────────────────────────────────────────────── */
void bufcache_stats(int *hits, int *misses, int *writes);

/* Enhanced stats: total accesses, evictions, dirty forced writes, working-set estimate */
void bufcache_stats_ex(uint64_t *total_accesses, uint64_t *evictions,
                        uint64_t *dirty_forced_writes, uint32_t *ws_est);

#endif /* BUFCACHE_H */
