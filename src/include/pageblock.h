#ifndef PAGEBLOCK_H
#define PAGEBLOCK_H

#include "types.h"

/*
 * pageblock.h — Pageblock migration types for anti-fragmentation
 *
 * Physical memory is divided into pageblocks (2 MB each = 512 × 4 KB pages).
 * Each pageblock is tagged with a migration type that describes what kind
 * of allocations are expected to live there.  By segregating movable and
 * unmovable allocations we prevent long-term fragmentation: movable pages
 * can later be compacted (compaction already exists), while unmovable pages
 * are clustered together so they don't block higher-order allocations.
 *
 * Migration types (matching Linux conventions):
 *   UNMOVABLE   — kernel image, page tables, slab metadata; cannot migrate
 *   MOVABLE     — anonymous pages, page cache, most userspace; can relocate
 *   RECLAIMABLE — slab caches, dentries, inodes; can be freed (not migrated)
 *   CMA         — Contiguous Memory Allocator pool; movable use while free
 *   ISOLATE     — temporarily isolated for migration/offlining
 */

/* ── Pageblock constants ────────────────────────────────────────────── */

/* Pageblock order: 9 → 2 MB (512 × 4 KB pages) */
#define PAGEBLOCK_ORDER      9
#define PAGEBLOCK_NR_PAGES   (1UL << PAGEBLOCK_ORDER)   /* 512 */
#define PAGEBLOCK_SIZE       (PAGEBLOCK_NR_PAGES * (uint64_t)PAGE_SIZE)  /* 2 MB */

/* Maximum number of pageblocks (for 8 GB of RAM: 4096) */
#define PAGEBLOCK_MAX        4096

/* ── Migration types ──────────────────────────────────────────────────── */

enum migratetype {
    MIGRATE_UNMOVABLE   = 0,
    MIGRATE_MOVABLE     = 1,
    MIGRATE_RECLAIMABLE = 2,
    MIGRATE_CMA         = 3,
    MIGRATE_ISOLATE     = 4,  /* temporary — never allocated from directly */
    MIGRATE_TYPES       = 5,
};

/* Fallback order: when a pageblock of the preferred type is unavailable,
 * try these types (in order).  MIGRATE_CMA is not a fallback target for
 * non-CMA callers — it is reserved. */
extern const enum migratetype fallbacks[MIGRATE_TYPES][MIGRATE_TYPES];

/* ── Pageblock API ─────────────────────────────────────────────────────── */

/* Initialise pageblock tracking.  Called from pmm_init(). */
void pageblock_init(uint64_t total_frames);

/* Return the pageblock index for a given physical frame number. */
static inline uint64_t pageblock_of_frame(uint64_t frame)
{
    return frame >> PAGEBLOCK_ORDER;
}

/* Get/set the migration type of the pageblock containing @frame. */
enum migratetype pageblock_get_migratetype(uint64_t frame);
void pageblock_set_migratetype(uint64_t frame, enum migratetype mt);

/* Find a free frame in a pageblock of the given type, scanning from
 * @start_hint (a physical address).  Returns physical address or 0. */
uint64_t pageblock_alloc_from_type(enum migratetype mt, uint64_t start_hint);

/* Dump pageblock type statistics to the kernel log. */
void pageblock_dump_stats(void);

#endif /* PAGEBLOCK_H */
