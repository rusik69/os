/*
 * Buffer cache — LRU sector cache for FAT32 and other block-level users.
 *
 * ── Design Overview ────────────────────────────────────────────────────
 *
 * The buffer cache stores recently-accessed disk sectors (SECT_SIZE = 512
 * bytes each) in a fixed-size array of BC_CAPACITY (64) entries.  Entries
 * are indexed by (dev_id, LBA) via a hash table for O(1) lookups, and
 * ordered by access recency via a doubly-linked LRU list for O(1)
 * eviction.
 *
 * ── LRU List (doubly-linked) ───────────────────────────────────────────
 *
 *   g_lru_head                     g_lru_tail
 *       │                             │
 *       ▼  prev=-1     prev ◄───▸ next     prev ◄───▸ next=-1
 *   ┌───────┐ ◄───▸ ┌───────┐     ┌───────┐
 *   │ MRU   │       │ ...   │ ... │ LRU   │
 *   │ entry │       │       │     │ entry │
 *   └───────┘       └───────┘     └───────┘
 *
 * The LRU list is a doubly-linked list implemented as parallel arrays
 * (g_lru[BC_CAPACITY]) storing prev/next indices.  g_lru_head points to
 * the most-recently-used (MRU) entry and g_lru_tail points to the
 * least-recently-used (LRU) entry.  On every cache hit the touched entry
 * is moved to the head (lru_touch).  On eviction we start scanning from
 * the tail (coldest entries) to find a victim.
 *
 * ── Free Pool ──────────────────────────────────────────────────────────
 *
 * All empty slots live at the *tail* end of the LRU list, forming a "free
 * pool" linked together.  When g_count < BC_CAPACITY, the tail entry is
 * guaranteed to be unused (valid=0) and can be claimed directly without
 * calling evict_one().  When a slot is freed (via invalidate or read
 * failure), it is appended back to the tail.
 *
 * ── Hash Table ─────────────────────────────────────────────────────────
 *
 * A simple hash table with BC_HASH_SIZE (64) buckets provides O(1) lookup
 * by (dev_id, LBA).  The hash function HASH() XORs the LBA with a
 * shifted version of itself and the dev_id, then masks to the bucket
 * count.  Collisions are chained via hash_next indices (-1 = end of
 * chain).  On eviction the victim is removed from its hash chain.
 *
 * ── Eviction Policy ────────────────────────────────────────────────────
 *
 * evict_one() uses a two-pass algorithm:
 *
 *   Pass 1 — Prefer clean eviction:
 *     Scan the LRU list from tail (coldest) toward head.  Return the
 *     first entry that is NOT dirty AND has refcount == 0.  This avoids
 *     write-back overhead on the common (hot-clean) path.
 *
 *   Pass 2 — Force eviction with write-back:
 *     If no clean victim is found (all entries dirty), re-scan from tail
 *     and take the first entry with refcount == 0.  The dirty data is
 *     copied into an evict_wb struct so the caller can perform the
 *     block write AFTER releasing the spinlock (avoiding lock ordering
 *     issues with the block layer).
 *
 *   If ALL entries have non-zero refcount (pinned by callers), eviction
 *   fails and the cache returns -1, forcing the caller to fall back to
 *   direct I/O.
 *
 * ── Write-back & Dirty Tracking ────────────────────────────────────────
 *
 * Each entry has a dirty flag.  Entries are marked dirty via
 * bufcache_mark_dirty() or bufcache_write().  Dirty data is written back
 * when:
 *   1. The entry is evicted (forced write-back in evict_one pass 2).
 *   2. A per-device dirty threshold (BC_MAX_DIRTY_PER_DEV = 32) is
 *      exceeded (bufcache_throttle_writes).
 *   3. The caller explicitly calls bufcache_flush() / bufcache_flush_all().
 *   4. The cache is disabled (bufcache_disable).
 *
 * All write-back I/O is performed outside the g_bc_lock spinlock to
 * prevent deadlocks with the block layer (which may call back into the
 * buffer cache).
 *
 * ── Refcounting ────────────────────────────────────────────────────────
 *
 * bufcache_read() increments the entry's refcount to pin it in the cache
 * for the caller.  The caller must call bufcache_release() to decrement
 * the refcount when done with the data pointer.  An entry with
 * refcount > 0 cannot be evicted.  This ensures the returned data pointer
 * remains valid even if another thread triggers eviction.
 *
 * ── Working-Set Estimation ─────────────────────────────────────────────
 *
 * Each entry tracks an access_count (frequency).  g_ws_est is a running
 * estimate of the active working-set size.  When an entry's access_count
 * exceeds g_ws_est, the estimate is boosted exponentially:
 *   g_ws_est = (g_ws_est + 1) * 2
 * The estimate decays implicitly as cold entries' access_count falls
 * behind (the counter is reset to 0 on eviction).
 *
 * ── Thread Safety ──────────────────────────────────────────────────────
 *
 * All cache state is protected by g_bc_lock (a spinlock).  I/O operations
 * (disk reads/writes) are performed outside the lock to avoid blocking
 * other cache users during I/O.  A double-check pattern is used in
 * bufcache_read(): after I/O completes the lock is re-acquired and the
 * hash table is checked again to handle races where another thread
 * inserted the same sector while we were reading from disk.
 */
#include "bufcache.h"

#include "blockdev.h"
#include "printf.h"
#include "spinlock.h"
#include "string.h"

/* ── Constants ──────────────────────────────────────────────────────── */
#define BC_CAPACITY 64 /* number of sector slots */
#define BC_HASH_BITS 6 /* 64 buckets */
#define BC_HASH_SIZE (1U << BC_HASH_BITS)
#define HASH(lba, dev) \
    (((uint32_t)(lba) ^ ((uint32_t)(lba) >> BC_HASH_BITS) ^ (dev)) & (BC_HASH_SIZE - 1))

/* ── Per-entry structure ────────────────────────────────────────────── */
struct bc_entry {
    uint64_t lba;            /* sector address */
    uint8_t dev_id;          /* block device id */
    uint8_t valid;           /* 1 = holds valid data */
    uint8_t dirty;           /* 1 = modified, needs write-back */
    uint8_t lru_node;        /* index of this entry in LRU list */
    int16_t hash_next;       /* next entry in hash bucket chain (-1 = end) */
    uint16_t access_count;   /* access frequency counter (for working-set est.) */
    int refcount;            /* number of outstanding references to data[] */
    uint8_t data[SECT_SIZE]; /* cached sector data (512 bytes) */
};

/* ── LRU linked list (doubly linked via indices) ────────────────────── */
struct lru_node {
    int16_t prev; /* previous entry index, -1 = head */
    int16_t next; /* next entry index, -1 = tail */
};

/* ── Global state ───────────────────────────────────────────────────── */
static struct bc_entry g_entries[BC_CAPACITY];
static struct lru_node g_lru[BC_CAPACITY]; /* parallel array indexed same as g_entries */
static int16_t g_hash[BC_HASH_SIZE];       /* head of each hash bucket chain (-1 = empty) */
static int16_t g_lru_head;                 /* most recently used */
static int16_t g_lru_tail;                 /* least recently used */
static int g_initialized = 0;
static int g_active = 0;
static int g_count = 0; /* number of valid entries */
static spinlock_t g_bc_lock;

/* Stats */
static int g_hits = 0;
static int g_misses = 0;
static int g_writes = 0;

/* Enhanced stats */
static uint64_t g_total_accesses = 0;
static uint64_t g_evictions = 0;
static uint64_t g_dirty_forced_writes = 0;

/* ── Per-device dirty writeback throttle ───────────────────────────── */
#define BC_MAX_DIRTY_PER_DEV 32            /* max dirty buffers per device before throttling */
static uint8_t g_dirty_count_per_dev[256]; /* per-device dirty buffer counter */

/* Throttle: if a device has too many dirty buffers, flush them */
static void bufcache_throttle_writes(uint8_t dev_id) {
    if (g_dirty_count_per_dev[dev_id] >= BC_MAX_DIRTY_PER_DEV) {
        kprintf("[bufcache] writeback throttle: dev=%u has %u dirty buffers "
                "(limit=%u)\n",
                dev_id, g_dirty_count_per_dev[dev_id], BC_MAX_DIRTY_PER_DEV);

        struct {
            uint64_t lba;
            uint8_t data[SECT_SIZE];
        } flush_bufs[BC_CAPACITY];
        int n_flush = 0;

        uint64_t irq_flags;
        spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);
        for (int i = 0; i < BC_CAPACITY; i++) {
            if (g_entries[i].valid && g_entries[i].dirty && g_entries[i].dev_id == dev_id &&
                g_entries[i].refcount == 0) {
                flush_bufs[n_flush].lba = g_entries[i].lba;
                memcpy(flush_bufs[n_flush].data, g_entries[i].data, SECT_SIZE);
                g_entries[i].dirty = 0;
                n_flush++;
            }
        }
        g_dirty_count_per_dev[dev_id] = 0;
        spinlock_irqsave_release(&g_bc_lock, irq_flags);

        int flushed = 0;
        for (int i = 0; i < n_flush; i++) {
            if (blockdev_write_sectors(dev_id, flush_bufs[i].lba, 1, flush_bufs[i].data) == 0) {
                g_writes++;
                flushed++;
            }
        }

        if (flushed > 0) {
            kprintf("[bufcache] throttled writeback: flushed %d buffers for dev=%u\n", flushed,
                    dev_id);
        }
    }
}

/* Working-set estimation: track access frequency per entry */
#define WS_DECAY_SHIFT 4      /* exponential decay factor */
static uint32_t g_ws_est = 0; /* working set estimate (active entries count) */

/* ── Forward declarations ───────────────────────────────────────────── */
struct evict_wb {
    int needs_write;
    uint8_t dev_id;
    uint64_t lba;
    uint8_t data[SECT_SIZE];
};

static void lru_touch(int16_t idx);
static void lru_remove(int16_t idx);
static void lru_push_head(int16_t idx);
static int16_t hash_lookup(uint64_t lba, uint8_t dev_id);
static void hash_remove(int16_t idx);
static void hash_insert(int16_t idx);
static int16_t evict_one(struct evict_wb *wb);

/* ── Initialization ─────────────────────────────────────────────────── */
void bufcache_init(void) {
    if (g_initialized)
        return;

    memset(g_entries, 0, sizeof(g_entries));
    memset(g_lru, 0, sizeof(g_lru));
    for (unsigned int i = 0; i < BC_HASH_SIZE; i++)
        g_hash[i] = -1;

    /* Initialize LRU doubly-linked list (all entries free, linked as a pool) */
    for (int i = 0; i < BC_CAPACITY; i++) {
        g_entries[i].lru_node = (uint8_t)i;
        g_entries[i].valid = 0;
        g_entries[i].hash_next = -1;
        g_lru[i].prev = (int16_t)(i - 1);
        g_lru[i].next = (int16_t)(i + 1);
    }
    g_lru[0].prev = -1;
    g_lru[BC_CAPACITY - 1].next = -1;
    /* All entries start in the free pool: head = 0, tail = BC_CAPACITY-1 */
    g_lru_head = 0;
    g_lru_tail = BC_CAPACITY - 1;

    spinlock_init(&g_bc_lock);
    g_initialized = 1;
}

void bufcache_enable(void) {
    g_active = 1;
}
void bufcache_disable(void) {
    g_active = 0;
    bufcache_flush_all();
}

/* ── Stats ──────────────────────────────────────────────────────────── */
void bufcache_stats(int *hits, int *misses, int *writes) {
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);
    if (hits)
        *hits = g_hits;
    if (misses)
        *misses = g_misses;
    if (writes)
        *writes = g_writes;
    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

/* ── LRU helpers ────────────────────────────────────────────────────── */
static void lru_touch(int16_t idx) {
    if (idx == g_lru_head)
        return; /* already MRU */
    lru_remove(idx);
    lru_push_head(idx);
}

static void lru_remove(int16_t idx) {
    struct lru_node *n = &g_lru[idx];
    if (n->prev >= 0)
        g_lru[n->prev].next = n->next;
    else
        g_lru_head = n->next;
    if (n->next >= 0)
        g_lru[n->next].prev = n->prev;
    else
        g_lru_tail = n->prev;
}

static void lru_push_head(int16_t idx) {
    struct lru_node *n = &g_lru[idx];
    n->prev = -1;
    n->next = g_lru_head;
    if (g_lru_head >= 0)
        g_lru[g_lru_head].prev = idx;
    g_lru_head = idx;
    if (g_lru_tail < 0)
        g_lru_tail = idx;
}

/* ── Hash table helpers ─────────────────────────────────────────────── */
static int16_t hash_lookup(uint64_t lba, uint8_t dev_id) {
    uint32_t bucket = HASH(lba, dev_id);
    int16_t idx = g_hash[bucket];
    int steps = 0;
    while (idx >= 0) {
        if (++steps > BC_CAPACITY + 2) {
            /* Hash chain cycle — corrupt.  Dump the chain and bail. */
            kprintf("[bc] HASH CYCLE! bucket=%u lba=%llu chain:", bucket, (unsigned long long)lba);
            int16_t c = g_hash[bucket];
            for (int i = 0; i < 80 && c >= 0; i++) {
                kprintf(" %d(lba=%llu)", (int)c, (unsigned long long)g_entries[c].lba);
                c = g_entries[c].hash_next;
            }
            kprintf("\n");
            return -1;
        }
        if (g_entries[idx].valid && g_entries[idx].lba == lba && g_entries[idx].dev_id == dev_id) {
            return idx;
        }
        idx = g_entries[idx].hash_next;
    }
    return -1;
}

static void hash_remove(int16_t idx) {
    struct bc_entry *e = &g_entries[idx];
    uint32_t bucket = HASH(e->lba, e->dev_id);
    int16_t *pp = &g_hash[bucket];
    int steps = 0;
    while (*pp >= 0) {
        /* Guard against a corrupted hash chain (cycle) — a bug elsewhere
         * must never hang the block layer.  If the entry is not found
         * within BC_CAPACITY+2 steps the chain is broken; bail out so
         * the caller's eviction/read can proceed via direct I/O. */
        if (++steps > BC_CAPACITY + 2) {
            kprintf("[bufcache] HASH REMOVE CYCLE idx=%d bucket=%u lba=%llu\n", (int)idx,
                    (unsigned int)bucket, (unsigned long long)e->lba);
            return;
        }
        if (*pp == idx) {
            *pp = e->hash_next;
            e->hash_next = -1;
            return;
        }
        pp = &g_entries[*pp].hash_next;
    }
}

static void hash_insert(int16_t idx) {
    struct bc_entry *e = &g_entries[idx];
    uint32_t bucket = HASH(e->lba, e->dev_id);
    e->hash_next = g_hash[bucket];
    g_hash[bucket] = idx;
}

/* ── Eviction ───────────────────────────────────────────────────────── */
static int16_t evict_one(struct evict_wb *wb) {
    if (wb)
        memset(wb, 0, sizeof(*wb));

    /* Start from tail (LRU) and work backwards until we find a clean, unreferenced entry */
    int16_t idx = g_lru_tail;
    int steps = 0;
    while (idx >= 0) {
        /* Guard against a corrupted LRU cycle (a node whose prev chain
         * loops back) — a bug elsewhere must never hang the block layer. */
        if (++steps > BC_CAPACITY + 2) {
            kprintf("[bufcache] LRU CYCLE in evict_one clean-pass! tail=%d\n", (int)g_lru_tail);
            break;
        }
        if (!g_entries[idx].dirty && g_entries[idx].refcount == 0) {
            /* Evict this clean entry */
            hash_remove(idx);
            g_entries[idx].valid = 0;
            g_count--;
            return idx;
        }
        idx = g_lru[idx].prev;
    }

    /* All entries are dirty or referenced — force-evict the LRU unreferenced entry */
    idx = g_lru_tail;
    steps = 0;
    while (idx >= 0) {
        if (++steps > BC_CAPACITY + 2) {
            kprintf("[bufcache] LRU CYCLE in evict_one force-pass! tail=%d\n", (int)g_lru_tail);
            break;
        }
        if (g_entries[idx].refcount == 0) {
            struct bc_entry *e = &g_entries[idx];
            hash_remove(idx);

            /* Copy dirty data for caller to write back outside the lock */
            if (e->dirty && wb) {
                wb->needs_write = 1;
                wb->dev_id = e->dev_id;
                wb->lba = e->lba;
                memcpy(wb->data, e->data, SECT_SIZE);
                g_dirty_forced_writes++;
            }

            e->valid = 0;
            e->dirty = 0;
            e->access_count = 0; /* reset on eviction */
            g_evictions++;
            g_count--;
            return idx;
        }
        idx = g_lru[idx].prev;
    }

    return -1; /* all entries are pinned */
}

/* ── Core cache operations ──────────────────────────────────────────── */

/* ── Public API ─────────────────────────────────────────────────────── */

void *bufcache_read(uint64_t lba, uint8_t dev_id) {
    if (!g_active || !g_initialized) {
        /* Fallthrough: caller uses direct I/O */
        return NULL;
    }

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    /* Check cache */
    int16_t idx = hash_lookup(lba, dev_id);
    if (idx >= 0) {
        /* Cache hit — touch LRU, increment access count, and return data pointer */
        lru_touch(idx);
        g_hits++;
        g_total_accesses++;
        g_entries[idx].access_count++;
        g_entries[idx].refcount++; /* pin buffer for caller */
        /* Update working set estimate */
        if (g_entries[idx].access_count > g_ws_est)
            g_ws_est = (g_ws_est + 1) * 2;
        spinlock_irqsave_release(&g_bc_lock, irq_flags);
        return g_entries[idx].data;
    }

    g_misses++;
    g_total_accesses++;

    /* Cache miss — need to fill */
    struct evict_wb wb = {0};
    int16_t victim;
    int was_evicted = 0;

    if (g_count < BC_CAPACITY) {
        /* Use the LRU tail (free pool entry) — but only if it is really
         * a pool entry.  If the count accounting ever drifts (see the
         * was_evicted increment below), the LRU tail can be an ACTIVE
         * entry; reusing it without hash_remove corrupts the hash chain
         * (observed: infinite loop in hash_lookup during the module
         * read).  Fall through to the eviction path in that case. */
        int16_t pool_tail = g_lru_tail;
        if (pool_tail >= 0 && !g_entries[pool_tail].valid) {
            victim = pool_tail;
            lru_remove(victim);
        } else {
            victim = evict_one(&wb);
            if (victim < 0) {
                spinlock_irqsave_release(&g_bc_lock, irq_flags);
                return NULL;
            }
            was_evicted = 1;
            lru_remove(victim);
        }
    } else {
        /* Evict an existing entry */
        victim = evict_one(&wb);
        if (victim < 0) {
            spinlock_irqsave_release(&g_bc_lock, irq_flags);
            return NULL;
        }
        was_evicted = 1;
        lru_remove(victim);
    }

    struct bc_entry *e = &g_entries[victim];
    e->lba = lba;
    e->dev_id = dev_id;
    e->valid = 0; /* not valid until read completes */
    e->dirty = 0;
    e->access_count = 1;
    e->refcount = 0; /* no outstanding references yet */

    spinlock_irqsave_release(&g_bc_lock, irq_flags);

    /* Write back evicted dirty data (outside the lock) */
    if (wb.needs_write) {
        kprintf("[bufcache] evict-writeback lba=%llu dev=%u\n", (unsigned long long)wb.lba,
                (unsigned int)wb.dev_id);
        int wr = blockdev_write_sectors(wb.dev_id, wb.lba, 1, wb.data);
        kprintf("[bufcache] evict-writeback lba=%llu rc=%d\n", (unsigned long long)wb.lba, wr);
        g_writes++;
    }

    /* Read from disk (outside the lock — avoids deadlock on async devices) */
    if (blk_submit_sync(dev_id, lba, 1, e->data, BLK_REQ_READ) != 0) {
        /* Read failed — return victim slot to free pool.
         * g_count stays decremented for an evicted victim (its valid
         * entry is gone); the slot is re-counted when it is reused. */
        spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);
        e->valid = 0;
        g_lru[victim].prev = g_lru_tail;
        g_lru[victim].next = -1;
        if (g_lru_tail >= 0)
            g_lru[g_lru_tail].next = (int16_t)victim;
        g_lru_tail = (int16_t)victim;
        if (g_lru_head < 0)
            g_lru_head = (int16_t)victim;
        spinlock_irqsave_release(&g_bc_lock, irq_flags);
        return NULL;
    }

    /* Re-acquire lock to finalize */
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    /* Check if another thread cached this sector while we were doing I/O */
    int16_t existing = hash_lookup(lba, dev_id);
    if (existing >= 0) {
        /* Another thread beat us — free our victim slot and return their entry */
        e->valid = 0;
        g_lru[victim].prev = g_lru_tail;
        g_lru[victim].next = -1;
        if (g_lru_tail >= 0)
            g_lru[g_lru_tail].next = (int16_t)victim;
        g_lru_tail = (int16_t)victim;
        if (g_lru_head < 0)
            g_lru_head = (int16_t)victim;

        g_entries[existing].refcount++;
        spinlock_irqsave_release(&g_bc_lock, irq_flags);
        return g_entries[existing].data;
    }

    /* We own this slot — finalize insertion */
    e->valid = 1;
    hash_insert(victim);
    lru_push_head(victim);
    /* Count one valid entry.  evict_one already decremented g_count for
     * the evicted victim; the new entry replaces it, so the count must
     * always come back up — the old `if (!was_evicted)` guard let the
     * count drift down, which made the free-pool path above reuse ACTIVE
     * entries and corrupt the hash chain (infinite hash_lookup). */
    g_count++;
    e->refcount++;
    spinlock_irqsave_release(&g_bc_lock, irq_flags);
    return e->data;
}

/* Release a previously acquired buffer cache entry.
 * Decrements the refcount, allowing the entry to be evicted later. */
void bufcache_release(uint64_t lba, uint8_t dev_id) {
    if (!g_active || !g_initialized)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    int16_t idx = hash_lookup(lba, dev_id);
    if (idx >= 0 && g_entries[idx].refcount > 0) {
        g_entries[idx].refcount--;
    }

    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

int bufcache_mark_dirty(uint64_t lba, uint8_t dev_id) {
    if (!g_active || !g_initialized)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    int16_t idx = hash_lookup(lba, dev_id);
    if (idx < 0) {
        spinlock_irqsave_release(&g_bc_lock, irq_flags);
        return -1;
    }

    g_entries[idx].dirty = 1;
    g_dirty_count_per_dev[dev_id]++;
    lru_touch(idx);
    spinlock_irqsave_release(&g_bc_lock, irq_flags);

    /* Throttle: flush if too many dirty buffers on this device */
    bufcache_throttle_writes(dev_id);
    return 0;
}

int bufcache_write(uint64_t lba, uint8_t dev_id, const void *data) {
    if (!g_active || !g_initialized) {
        /* Fallthrough: direct write */
        return blk_submit_sync(dev_id, lba, 1, (void *)(uintptr_t)data, BLK_REQ_WRITE);
    }

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    int16_t idx = hash_lookup(lba, dev_id);
    if (idx >= 0) {
        /* Update in-place */
        struct bc_entry *e = &g_entries[idx];
        memcpy(e->data, data, SECT_SIZE);
        e->dirty = 1;
        g_dirty_count_per_dev[dev_id]++;
        lru_touch(idx);
        g_writes++;
        g_total_accesses++;
        e->access_count++;
        spinlock_irqsave_release(&g_bc_lock, irq_flags);
        return 0;
    }

    /* Cache miss — need to insert new entry */
    struct evict_wb wb = {0};
    int16_t victim;
    int was_evicted = 0;

    if (g_count < BC_CAPACITY) {
        /* Use the LRU tail (free pool entry) — but only if it is really
         * a pool entry.  If the count accounting ever drifts (see the
         * was_evicted increment below), the LRU tail can be an ACTIVE
         * entry; reusing it without hash_remove corrupts the hash chain
         * (same hazard as bufcache_read's pool path). */
        int16_t pool_tail = g_lru_tail;
        if (pool_tail >= 0 && !g_entries[pool_tail].valid) {
            victim = pool_tail;
            lru_remove(victim);
        } else {
            victim = evict_one(&wb);
            if (victim < 0) {
                /* Cache full with dirty entries — write directly */
                spinlock_irqsave_release(&g_bc_lock, irq_flags);
                return blk_submit_sync(dev_id, lba, 1, (void *)(uintptr_t)data,
                                       BLK_REQ_WRITE);
            }
            was_evicted = 1;
            lru_remove(victim);
        }
    } else {
        victim = evict_one(&wb);
        if (victim < 0) {
            /* Cache full with dirty entries — write directly */
            spinlock_irqsave_release(&g_bc_lock, irq_flags);
            return blk_submit_sync(dev_id, lba, 1, (void *)(uintptr_t)data, BLK_REQ_WRITE);
        }
        was_evicted = 1;
        lru_remove(victim);
    }

    struct bc_entry *e = &g_entries[victim];
    e->lba = lba;
    e->dev_id = dev_id;
    e->valid = 1;
    e->dirty = 1;
    g_dirty_count_per_dev[dev_id]++;
    memcpy(e->data, data, SECT_SIZE);
    hash_insert(victim);
    lru_push_head(victim);
    /* Count one valid entry.  evict_one already decremented g_count for
     * the evicted victim; the new entry replaces it, so the count must
     * always come back up — the old `if (!was_evicted)` guard let the
     * count drift down, which made the free-pool path above reuse ACTIVE
     * entries and corrupt the hash chain (infinite hash_lookup). */
    g_count++;

    spinlock_irqsave_release(&g_bc_lock, irq_flags);

    /* Write back evicted dirty data (outside the lock) */
    if (wb.needs_write) {
        blockdev_write_sectors(wb.dev_id, wb.lba, 1, wb.data);
        g_writes++;
    }

    /* Throttle: flush if too many dirty buffers on this device */
    bufcache_throttle_writes(dev_id);
    return 0;
}

void bufcache_flush(void) {
    if (!g_initialized)
        return;

    struct {
        uint8_t dev_id;
        uint64_t lba;
        uint8_t data[SECT_SIZE];
    } flush_bufs[BC_CAPACITY];
    int n_flush = 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    for (int i = 0; i < BC_CAPACITY; i++) {
        if (g_entries[i].valid && g_entries[i].dirty) {
            flush_bufs[n_flush].dev_id = g_entries[i].dev_id;
            flush_bufs[n_flush].lba = g_entries[i].lba;
            memcpy(flush_bufs[n_flush].data, g_entries[i].data, SECT_SIZE);
            g_entries[i].dirty = 0;
            n_flush++;
        }
    }

    spinlock_irqsave_release(&g_bc_lock, irq_flags);

    for (int i = 0; i < n_flush; i++) {
        blockdev_write_sectors(flush_bufs[i].dev_id, flush_bufs[i].lba, 1, flush_bufs[i].data);
        g_writes++;
    }
}

void bufcache_flush_all(void) {
    bufcache_flush();
}

/* ── Flush dirty entries for a specific device ──────────────────────── */

void bufcache_flush_dev(uint8_t dev_id) {
    if (!g_initialized)
        return;

    struct {
        uint64_t lba;
        uint8_t data[SECT_SIZE];
    } flush_bufs[BC_CAPACITY];
    int n_flush = 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    for (int i = 0; i < BC_CAPACITY; i++) {
        if (g_entries[i].valid && g_entries[i].dirty && g_entries[i].dev_id == dev_id) {
            flush_bufs[n_flush].lba = g_entries[i].lba;
            memcpy(flush_bufs[n_flush].data, g_entries[i].data, SECT_SIZE);
            g_entries[i].dirty = 0;
            n_flush++;
        }
    }

    spinlock_irqsave_release(&g_bc_lock, irq_flags);

    for (int i = 0; i < n_flush; i++) {
        blockdev_write_sectors(dev_id, flush_bufs[i].lba, 1, flush_bufs[i].data);
        g_writes++;
    }
}

/* ── Writeback: flush dirty pages without invalidating ──────────────── */

int bufcache_writeback(void) {
    if (!g_initialized)
        return -1;

    struct {
        uint8_t dev_id;
        uint64_t lba;
        uint8_t data[SECT_SIZE];
    } flush_bufs[BC_CAPACITY];
    int n_flush = 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    /* Walk the LRU list from tail (coldest) to head (hottest) */
    int16_t idx = g_lru_tail;
    while (idx >= 0) {
        if (g_entries[idx].valid && g_entries[idx].dirty) {
            flush_bufs[n_flush].dev_id = g_entries[idx].dev_id;
            flush_bufs[n_flush].lba = g_entries[idx].lba;
            memcpy(flush_bufs[n_flush].data, g_entries[idx].data, SECT_SIZE);
            g_entries[idx].dirty = 0;
            n_flush++;
        }
        idx = g_lru[idx].prev;
    }

    spinlock_irqsave_release(&g_bc_lock, irq_flags);

    for (int i = 0; i < n_flush; i++) {
        blockdev_write_sectors(flush_bufs[i].dev_id, flush_bufs[i].lba, 1, flush_bufs[i].data);
        g_writes++;
    }

    return n_flush;
}

void bufcache_set_dirty(uint64_t lba, uint8_t dev_id) {
    if (!g_active || !g_initialized)
        return;
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);
    int16_t idx = hash_lookup(lba, dev_id);
    if (idx >= 0) {
        g_entries[idx].dirty = 1;
        lru_touch(idx);
    }
    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

void bufcache_clear_dirty(uint64_t lba, uint8_t dev_id) {
    if (!g_active || !g_initialized)
        return;
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);
    int16_t idx = hash_lookup(lba, dev_id);
    if (idx >= 0) {
        g_entries[idx].dirty = 0;
    }
    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

void bufcache_invalidate(uint64_t lba, uint8_t dev_id) {
    if (!g_active || !g_initialized)
        return;

    uint64_t irq_flags;
    uint8_t needs_write = 0;
    uint8_t data_copy[SECT_SIZE];

    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    int16_t idx = hash_lookup(lba, dev_id);
    if (idx < 0) {
        spinlock_irqsave_release(&g_bc_lock, irq_flags);
        return;
    }

    struct bc_entry *e = &g_entries[idx];
    /* Cannot invalidate a pinned buffer */
    if (e->refcount > 0) {
        spinlock_irqsave_release(&g_bc_lock, irq_flags);
        return;
    }

    if (e->dirty) {
        memcpy(data_copy, e->data, SECT_SIZE);
        e->dirty = 0;
        needs_write = 1;
    }
    hash_remove(idx);
    e->valid = 0;
    lru_remove(idx);
    /* Return to free pool at tail */
    g_lru[idx].prev = g_lru_tail;
    g_lru[idx].next = -1;
    if (g_lru_tail >= 0)
        g_lru[g_lru_tail].next = (int16_t)idx;
    g_lru_tail = (int16_t)idx;
    if (g_lru_head < 0)
        g_lru_head = (int16_t)idx;
    g_count--;

    spinlock_irqsave_release(&g_bc_lock, irq_flags);

    if (needs_write) {
        blk_submit_sync(dev_id, lba, 1, data_copy, BLK_REQ_WRITE);
        g_writes++;
    }
}

/* Enhanced stats */
void bufcache_stats_ex(uint64_t *total_accesses, uint64_t *evictions, uint64_t *dirty_forced_writes,
                       uint32_t *ws_est) {
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);
    if (total_accesses)
        *total_accesses = g_total_accesses;
    if (evictions)
        *evictions = g_evictions;
    if (dirty_forced_writes)
        *dirty_forced_writes = g_dirty_forced_writes;
    if (ws_est)
        *ws_est = g_ws_est;
    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}
