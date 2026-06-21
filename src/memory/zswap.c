/*
 * zswap.c — Compressed in-memory swap cache (Item 224)
 *
 * Implements a compressed cache for swapped-out kernel pages using the
 * zcomp compression framework.  On swap-out, we try to compress the page
 * and store the result in a memory pool.  On swap-in, we check the pool
 * first and decompress if found, avoiding disk I/O.
 *
 * Architecture:
 *   - Hash table keyed by (dev_idx, slot) with chained overflow
 *   - Per-entry compressed buffer allocated via kmalloc
 *   - Pool size limit as % of total RAM (default 5%)
 *   - Compression via zcomp_fast (LZ77, optimized for 4K pages)
 *   - Per-CPU compression streams for concurrent access
 *
 * Reference: Linux zswap design (simplified for this kernel)
 */
#define KERNEL_INTERNAL
#include "zswap.h"
#include "zcomp.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "pmm.h"
#include "smp.h"

/* ── Hash table entry ───────────────────────────────────────────────── */

struct zswap_entry {
    int      in_use;            /* 1 = slot occupied               */
    int      dev_idx;           /* swap device index               */
    uint32_t slot;              /* swap slot index                 */
    uint8_t *comp_data;         /* kmalloc'd compressed buffer     */
    int      comp_len;          /* compressed data length (bytes)  */
    struct zswap_entry *next;   /* next entry in hash chain        */
};

/* ── Global state ───────────────────────────────────────────────────── */

/** Hash table: array of linked-list heads. */
static struct zswap_entry *zswap_table[ZSWAP_HASH_SIZE];

/** Per-CPU compression streams. */
static struct zcomp_stream *zswap_streams;
static int  zswap_num_cpus;
static const struct zcomp_ops *zswap_algo;

/** Spinlock protecting the hash table. */
static spinlock_t zswap_lock = SPINLOCK_INIT;

/** Pool capacity and usage tracking. */
static uint64_t zswap_pool_max;     /* max total compressed bytes   */
static uint64_t zswap_pool_used;    /* current compressed bytes     */
static uint32_t zswap_entry_count;  /* number of entries in pool    */
static int      zswap_initialised;  /* 1 = zswap is ready           */

/* ── Hash helpers ───────────────────────────────────────────────────── */

/** Jenkins one-at-a-time hash of (dev_idx, slot) -> table index. */
static uint32_t zswap_hash(int dev_idx, uint32_t slot)
{
    uint32_t h = 0;
    uint8_t  buf[8];
    buf[0] = (uint8_t)(dev_idx & 0xFF);
    buf[1] = (uint8_t)((dev_idx >> 8) & 0xFF);
    buf[2] = (uint8_t)(slot & 0xFF);
    buf[3] = (uint8_t)((slot >> 8) & 0xFF);
    buf[4] = (uint8_t)((slot >> 16) & 0xFF);
    buf[5] = (uint8_t)((slot >> 24) & 0xFF);
    buf[6] = 0;
    buf[7] = 0;

    for (int i = 0; i < 6; i++) {
        h += buf[i];
        h += (h << 10);
        h ^= (h >> 6);
    }
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);
    return h % ZSWAP_HASH_SIZE;
}

/* ── Entry lookup ───────────────────────────────────────────────────── */

static struct zswap_entry *zswap_find(int dev_idx, uint32_t slot)
{
    uint32_t idx = zswap_hash(dev_idx, slot);
    struct zswap_entry *e = zswap_table[idx];
    while (e) {
        if (e->in_use && e->dev_idx == dev_idx && e->slot == slot)
            return e;
        e = e->next;
    }
    return NULL;
}

/* ── Initialization ─────────────────────────────────────────────────── */

void zswap_init(void)
{
    if (zswap_initialised)
        return;

    /* Initialise hash table */
    memset(zswap_table, 0, sizeof(zswap_table));

    /* Ensure compression subsystem is initialised (zcomp_init called earlier) */
    zswap_algo = zcomp_find(ZCOMP_ALGO_FAST);
    if (!zswap_algo) {
        kprintf("[zswap] WARNING: no compression algorithm available — "
                "zswap disabled\n");
        return;
    }

    /* Determine number of CPUs */
    extern int smp_get_cpu_count(void);
    zswap_num_cpus = smp_get_cpu_count();
    if (zswap_num_cpus < 1)
        zswap_num_cpus = 1;

    /* Allocate per-CPU compression streams */
    zswap_streams = (struct zcomp_stream *)
        kmalloc((size_t)zswap_num_cpus * sizeof(struct zcomp_stream));
    if (!zswap_streams) {
        kprintf("[zswap] ERROR: failed to allocate compression streams\n");
        return;
    }
    memset(zswap_streams, 0,
           (size_t)zswap_num_cpus * sizeof(struct zcomp_stream));

    if (zcomp_streams_init(zswap_streams, zswap_num_cpus, zswap_algo) < 0) {
        kprintf("[zswap] ERROR: failed to init compression streams\n");
        kfree(zswap_streams);
        zswap_streams = NULL;
        return;
    }

    /* Calculate pool size limit: % of total RAM */
    uint64_t total_pages = pmm_get_total_frames();
    uint64_t pool_pages  = total_pages * ZSWAP_DEFAULT_POOL_PCT / 100;
    if (pool_pages < 1)
        pool_pages = 1;

    zswap_pool_max = pool_pages * 4096;   /* in bytes */
    zswap_pool_used = 0;
    zswap_entry_count = 0;
    zswap_initialised = 1;

    kprintf("[zswap] Compressed swap cache initialised: algorithm=%s, "
            "pool_max=%llu KB (%u%% of %llu pages)\n",
            zswap_algo->name,
            (unsigned long long)(zswap_pool_max / 1024),
            ZSWAP_DEFAULT_POOL_PCT,
            (unsigned long long)total_pages);
}

/* ── Store (compress and cache) ─────────────────────────────────────── */

int zswap_store(uint64_t phys_addr, int dev_idx, uint32_t slot)
{
    if (!zswap_initialised || !zswap_algo || !zswap_streams)
        return -1;

    /* Check if pool is full */
    if (zswap_is_full())
        return -1;

    /* Check if entry already exists (shouldn't happen, but guard) */
    spinlock_acquire(&zswap_lock);
    struct zswap_entry *existing = zswap_find(dev_idx, slot);
    spinlock_release(&zswap_lock);
    if (existing)
        return -1;  /* Already stored — caller should not retry */

    /* Allocate compression output buffer (worst case) */
    uint8_t *comp_buf = (uint8_t *)kmalloc(ZSWAP_MAX_COMP_LEN);
    if (!comp_buf)
        return -1;

    /* Get per-CPU compression stream */
    struct zcomp_stream *zs = zcomp_stream_get(zswap_streams, zswap_num_cpus);

    /* Get virtual address of the physical page */
    uint64_t virt_addr = phys_addr + 0xFFFF800000000000ULL;
    const uint8_t *page_data = (const uint8_t *)(uintptr_t)virt_addr;

    /* Compress the 4K page */
    int comp_len = zcomp_stream_compress(zs, page_data, 4096,
                                          comp_buf, ZSWAP_MAX_COMP_LEN);
    if (comp_len <= 0) {
        /* Compression failed or data is incompressible */
        kfree(comp_buf);
        return -1;
    }

    /* Reject if compression didn't save at least one page (no benefit) */
    if ((size_t)comp_len >= 4096) {
        kfree(comp_buf);
        return -1;
    }

    /* Allocate entry */
    struct zswap_entry *entry = (struct zswap_entry *)
        kmalloc(sizeof(struct zswap_entry));
    if (!entry) {
        kfree(comp_buf);
        return -1;
    }

    /* Shrink the compressed buffer to exact size */
    uint8_t *shrunk = (uint8_t *)kmalloc((size_t)comp_len);
    if (!shrunk) {
        /* Keep the over-allocated buffer; not ideal but functional */
        shrunk = comp_buf;
    } else {
        memcpy(shrunk, comp_buf, (size_t)comp_len);
        kfree(comp_buf);
    }

    /* Fill in entry */
    entry->in_use   = 1;
    entry->dev_idx  = dev_idx;
    entry->slot     = slot;
    entry->comp_data = shrunk;
    entry->comp_len = comp_len;
    entry->next     = NULL;

    /* Insert into hash table */
    spinlock_acquire(&zswap_lock);

    uint32_t idx = zswap_hash(dev_idx, slot);
    entry->next = zswap_table[idx];
    zswap_table[idx] = entry;

    zswap_pool_used += (uint64_t)comp_len +
                       (uint64_t)sizeof(struct zswap_entry);
    zswap_entry_count++;

    spinlock_release(&zswap_lock);

    return 0;
}

/* ── Load (decompress from cache) ───────────────────────────────────── */

int zswap_load(int dev_idx, uint32_t slot, uint64_t phys_addr)
{
    if (!zswap_initialised || !zswap_algo || !zswap_streams)
        return -1;

    spinlock_acquire(&zswap_lock);

    struct zswap_entry *entry = zswap_find(dev_idx, slot);
    if (!entry || !entry->in_use) {
        spinlock_release(&zswap_lock);
        return -1;
    }

    /* Save pointers before removing entry */
    uint8_t *comp_data = entry->comp_data;
    int      comp_len  = entry->comp_len;

    /* Remove entry from hash table */
    uint32_t idx = zswap_hash(dev_idx, slot);
    struct zswap_entry **pp = &zswap_table[idx];
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            break;
        }
        pp = &(*pp)->next;
    }

    /* Update pool counters */
    zswap_pool_used -= (uint64_t)comp_len +
                       (uint64_t)sizeof(struct zswap_entry);
    zswap_entry_count--;

    spinlock_release(&zswap_lock);

    /* Get virtual address of destination physical page */
    uint64_t virt_addr = phys_addr + 0xFFFF800000000000ULL;
    uint8_t *page_data = (uint8_t *)(uintptr_t)virt_addr;

    /* Get per-CPU decompression stream */
    struct zcomp_stream *zs = zcomp_stream_get(zswap_streams, zswap_num_cpus);

    /* Decompress */
    int ret = zcomp_stream_decompress(zs, comp_data, (size_t)comp_len,
                                       page_data, 4096);

    /* Free the entry and compressed buffer */
    kfree(comp_data);
    kfree(entry);

    return (ret > 0) ? 0 : -1;
}

/* ── Free entry (without decompressing) ─────────────────────────────── */

void zswap_free(int dev_idx, uint32_t slot)
{
    if (!zswap_initialised)
        return;

    spinlock_acquire(&zswap_lock);

    struct zswap_entry *entry = zswap_find(dev_idx, slot);
    if (!entry || !entry->in_use) {
        spinlock_release(&zswap_lock);
        return;
    }

    /* Remove from hash table */
    uint32_t idx = zswap_hash(dev_idx, slot);
    struct zswap_entry **pp = &zswap_table[idx];
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            break;
        }
        pp = &(*pp)->next;
    }

    /* Update pool counters */
    zswap_pool_used -= (uint64_t)entry->comp_len +
                       (uint64_t)sizeof(struct zswap_entry);
    zswap_entry_count--;

    spinlock_release(&zswap_lock);

    /* Free resources */
    if (entry->comp_data)
        kfree(entry->comp_data);
    kfree(entry);
}

/* ── Pool capacity check ────────────────────────────────────────────── */

int zswap_is_full(void)
{
    if (!zswap_initialised)
        return 1;
    return (zswap_pool_used >= zswap_pool_max) ? 1 : 0;
}

/* ── Statistics ─────────────────────────────────────────────────────── */

void zswap_stats(uint32_t *out_pages, uint32_t *out_size_kb)
{
    spinlock_acquire(&zswap_lock);
    if (out_pages)
        *out_pages = zswap_entry_count;
    if (out_size_kb)
        *out_size_kb = (uint32_t)(zswap_pool_used / 1024);
    spinlock_release(&zswap_lock);
}

/* ── Debug dump (enabled via dynamic debug) ─────────────────────────── */

void zswap_dump(void)
{
    if (!zswap_initialised) {
        kprintf("[zswap] not initialised\n");
        return;
    }

    spinlock_acquire(&zswap_lock);
    kprintf("[zswap] entries=%u  pool_used=%llu KB  pool_max=%llu KB  "
            "algo=%s\n",
            zswap_entry_count,
            (unsigned long long)(zswap_pool_used / 1024),
            (unsigned long long)(zswap_pool_max / 1024),
            zswap_algo ? zswap_algo->name : "none");

    /* List first 20 entries */
    int shown = 0;
    for (uint32_t i = 0; i < ZSWAP_HASH_SIZE && shown < 20; i++) {
        struct zswap_entry *e = zswap_table[i];
        while (e && shown < 20) {
            if (e->in_use) {
                kprintf("  [%d] dev=%d slot=%u comp_len=%d ratio=%u%%\n",
                        shown, e->dev_idx, e->slot, e->comp_len,
                        (unsigned int)(e->comp_len * 100 / 4096));
                shown++;
            }
            e = e->next;
        }
    }
    spinlock_release(&zswap_lock);
}
#include "module.h"
module_init(zswap_init);

/* ── Stub: zswap_invalidate ──────────────────────────────────── */
int zswap_invalidate(uint64_t offset)
{
    (void)offset;
    kprintf("[zswap] zswap_invalidate: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: zswap_shrink ──────────────────────────────────────── */
int zswap_shrink(int nr_to_reclaim)
{
    (void)nr_to_reclaim;
    kprintf("[zswap] zswap_shrink: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: zswap_writeback_entry ─────────────────────────────── */
int zswap_writeback_entry(uint64_t offset)
{
    (void)offset;
    kprintf("[zswap] zswap_writeback_entry: not yet implemented\n");
    return -ENOSYS;
}
