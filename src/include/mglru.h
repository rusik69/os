#ifndef MGLRU_H
#define MGLRU_H

#include "types.h"
#include "list.h"
#include "spinlock.h"

/* ── Multi-Generational LRU (MGLRU) Page Reclaim ─────────────────────
 *
 * Implements a 4-generation LRU scheme for page reclaim decisions.
 * New/accessed pages go into the youngest generation (3); pages age
 * towards generation 0 and are evicted from the oldest generation.
 *
 * Static arrays only — no dynamic allocation after init.
 */

#define MGLRU_NR_GENS     4
#define MGLRU_NR_NODES    4
#define MGLRU_MAX_PAGES   8192

/* ── Generation descriptor ──────────────────────────────────────────── */
struct mglru_gen {
    struct list_head list;         /* LRU list of pages in this generation */
    unsigned long nr_active;       /* active page count */
    unsigned long nr_inactive;     /* inactive page count */
    unsigned long nr_pages;        /* total pages in this generation */
};

/* ── Per-node MGLRU state ───────────────────────────────────────────── */
struct mglru_state {
    struct mglru_gen gens[MGLRU_NR_GENS];
    int              last_accessed_gen;   /* current generation index */
    int              last_evicted_gen;    /* generation being evicted from */
    spinlock_t       lock;
    unsigned long    aging_threshold;     /* threshold in jiffies */
    unsigned long    min_ttl_ms;          /* min time-to-live in ms */
    int              enabled;
} __cacheline_aligned;

/* ── Internal page tracking entry ────────────────────────────────────── */
struct mglru_page_entry {
    struct list_head list;         /* link in generation LRU list */
    uint64_t         phys_addr;    /* physical address of the page */
    int              gen;          /* current generation index */
    int              accessed;     /* young flag — promotes on next scan */
    int              in_use;       /* slot occupied */
};

/* ── Core API ───────────────────────────────────────────────────────── */

/* Initialise MGLRU subsystem (generations, default threshold, sysfs) */
void mglru_init(void);
void mglru_tick(void);

/* Advance the current generation, wrapping at MGLRU_NR_GENS (4) */
void mglru_age(void);

/* Isolate up to nr_to_isolate pages from the oldest evictable generation.
 * Removed pages are moved to the caller-provided list.
 * Returns the number of pages isolated (may be less than requested). */
int mglru_isolate(int nr_to_isolate, struct list_head *list);

/* Promote a page to a younger generation when accessed.
 * Called on page cache hits or page table access flag updates. */
void mglru_rotate(uint64_t phys_addr);

/* Reclaim up to nr_pages from the oldest generation.
 * Each reclaimed page is freed via pmm_free_frame.
 * gfp_mask is reserved for future use (e.g. __GFP_IO/FS).
 * Returns the number of pages successfully freed. */
int mglru_reclaim_pages(int nr_pages, unsigned int gfp_mask);

/* Add a newly allocated page to the youngest generation. */
void mglru_add_page(uint64_t phys_addr);

/* Remove a page from its generation (on free). */
void mglru_remove_page(uint64_t phys_addr);

/* Mark a page as recently accessed; sets a flag that promotes it
 * to a younger generation on the next aging scan. */
void mglru_page_accessed(uint64_t phys_addr);

/* Query per-generation page counts.  counts[] is filled in. */
void mglru_get_gen_counts(unsigned long counts[MGLRU_NR_GENS]);

/* Enable / disable / query MGLRU */
int  mglru_is_enabled(void);
void mglru_set_enabled(int val);

/* Get/set minimum time-to-live in milliseconds */
unsigned long mglru_get_min_ttl_ms(void);
void          mglru_set_min_ttl_ms(unsigned long ms);

/* Return the number of pages currently tracked across all generations */
unsigned long mglru_total_pages(void);

#endif /* MGLRU_H */
