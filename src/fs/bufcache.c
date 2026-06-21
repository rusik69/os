/*
 * Buffer cache — LRU sector cache for FAT32 and other block-level users.
 *
 * Caches recently-read sectors by (dev_id, lba) for fast reuse.
 * Evicts least-recently-used entries when full.
 * Tracks dirty entries for write-back.
 */
#include "bufcache.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

/* ── Constants ──────────────────────────────────────────────────────── */
#define BC_CAPACITY 64           /* number of sector slots */
#define BC_HASH_BITS 6           /* 64 buckets */
#define BC_HASH_SIZE (1 << BC_HASH_BITS)
#define HASH(lba, dev) (((uint32_t)(lba) ^ ((uint32_t)(lba) >> BC_HASH_BITS) ^ (dev)) & (BC_HASH_SIZE - 1))

/* ── Per-entry structure ────────────────────────────────────────────── */
struct bc_entry {
    uint64_t      lba;           /* sector address */
    uint8_t       dev_id;        /* block device id */
    uint8_t       valid;         /* 1 = holds valid data */
    uint8_t       dirty;         /* 1 = modified, needs write-back */
    uint8_t       lru_node;      /* index of this entry in LRU list */
    int16_t       hash_next;     /* next entry in hash bucket chain (-1 = end) */
    uint16_t      access_count;  /* access frequency counter (for working-set est.) */
    int           refcount;      /* number of outstanding references to data[] */
    uint8_t       data[SECT_SIZE]; /* cached sector data (512 bytes) */
};

/* ── LRU linked list (doubly linked via indices) ────────────────────── */
struct lru_node {
    int16_t prev;                /* previous entry index, -1 = head */
    int16_t next;                /* next entry index, -1 = tail */
};

/* ── Global state ───────────────────────────────────────────────────── */
static struct bc_entry g_entries[BC_CAPACITY];
static struct lru_node g_lru[BC_CAPACITY];   /* parallel array indexed same as g_entries */
static int16_t g_hash[BC_HASH_SIZE];          /* head of each hash bucket chain (-1 = empty) */
static int16_t g_lru_head;                   /* most recently used */
static int16_t g_lru_tail;                   /* least recently used */
static int g_initialized = 0;
static int g_active = 0;
static int g_count = 0;                      /* number of valid entries */
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
#define BC_MAX_DIRTY_PER_DEV 32   /* max dirty buffers per device before throttling */
static uint8_t  g_dirty_count_per_dev[256]; /* per-device dirty buffer counter */

/* Throttle: if a device has too many dirty buffers, flush them */
static void bufcache_throttle_writes(uint8_t dev_id) {
    if (g_dirty_count_per_dev[dev_id] >= BC_MAX_DIRTY_PER_DEV) {
        kprintf("[bufcache] writeback throttle: dev=%u has %u dirty buffers "
                "(limit=%u)\n",
                dev_id, g_dirty_count_per_dev[dev_id], BC_MAX_DIRTY_PER_DEV);

        /* Flush all dirty buffers for this device */
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);
        int flushed = 0;
        for (int i = 0; i < BC_CAPACITY; i++) {
            if (g_entries[i].valid && g_entries[i].dirty &&
                g_entries[i].dev_id == dev_id && g_entries[i].refcount == 0) {
                if (blockdev_write_sectors(dev_id, (uint32_t)g_entries[i].lba,
                                            1, g_entries[i].data) == 0) {
                    g_entries[i].dirty = 0;
                    g_writes++;
                    flushed++;
                }
            }
        }
        g_dirty_count_per_dev[dev_id] = 0;
        spinlock_irqsave_release(&g_bc_lock, irq_flags);

        if (flushed > 0) {
            kprintf("[bufcache] throttled writeback: flushed %d buffers for dev=%u\n",
                    flushed, dev_id);
        }
    }
}

/* Working-set estimation: track access frequency per entry */
#define WS_DECAY_SHIFT 4  /* exponential decay factor */
static uint32_t g_ws_est = 0;  /* working set estimate (active entries count) */

/* ── Forward declarations ───────────────────────────────────────────── */
static void lru_touch(int16_t idx);
static void lru_remove(int16_t idx);
static void lru_push_head(int16_t idx);
static int16_t hash_lookup(uint64_t lba, uint8_t dev_id);
static void hash_remove(int16_t idx);
static void hash_insert(int16_t idx);
static int16_t evict_one(void);

/* ── Initialization ─────────────────────────────────────────────────── */
void bufcache_init(void) {
    if (g_initialized) return;

    memset(g_entries, 0, sizeof(g_entries));
    memset(g_lru, 0, sizeof(g_lru));
    for (int i = 0; i < BC_HASH_SIZE; i++) g_hash[i] = -1;

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

void bufcache_enable(void) { g_active = 1; }
void bufcache_disable(void) {
    g_active = 0;
    bufcache_flush_all();
}

/* ── Stats ──────────────────────────────────────────────────────────── */
void bufcache_stats(int *hits, int *misses, int *writes) {
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);
    if (hits)   *hits   = g_hits;
    if (misses) *misses = g_misses;
    if (writes) *writes = g_writes;
    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

/* ── LRU helpers ────────────────────────────────────────────────────── */
static void lru_touch(int16_t idx) {
    if (idx == g_lru_head) return; /* already MRU */
    lru_remove(idx);
    lru_push_head(idx);
}

static void lru_remove(int16_t idx) {
    struct lru_node *n = &g_lru[idx];
    if (n->prev >= 0) g_lru[n->prev].next = n->next;
    else              g_lru_head = n->next;
    if (n->next >= 0) g_lru[n->next].prev = n->prev;
    else              g_lru_tail = n->prev;
}

static void lru_push_head(int16_t idx) {
    struct lru_node *n = &g_lru[idx];
    n->prev = -1;
    n->next = g_lru_head;
    if (g_lru_head >= 0) g_lru[g_lru_head].prev = idx;
    g_lru_head = idx;
    if (g_lru_tail < 0) g_lru_tail = idx;
}

/* ── Hash table helpers ─────────────────────────────────────────────── */
static int16_t hash_lookup(uint64_t lba, uint8_t dev_id) {
    uint32_t bucket = HASH(lba, dev_id);
    int16_t idx = g_hash[bucket];
    while (idx >= 0) {
        if (g_entries[idx].valid &&
            g_entries[idx].lba == lba &&
            g_entries[idx].dev_id == dev_id) {
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
    while (*pp >= 0) {
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
static int16_t evict_one(void) {
    /* Start from tail (LRU) and work backwards until we find a clean, unreferenced entry */
    int16_t idx = g_lru_tail;
    while (idx >= 0) {
        if (!g_entries[idx].dirty && g_entries[idx].refcount == 0) {
            /* Evict this clean entry */
            hash_remove(idx);
            g_entries[idx].valid = 0;
            return idx;
        }
        idx = g_lru[idx].prev;
    }

    /* All entries are dirty or referenced — force-write the LRU unreferenced entry */
    idx = g_lru_tail;
    while (idx >= 0) {
        if (g_entries[idx].refcount == 0) {
            struct bc_entry *e = &g_entries[idx];
            hash_remove(idx);

            /* Write dirty data back */
            if (e->dirty) {
                blockdev_write_sectors(e->dev_id, (uint32_t)e->lba, 1, e->data);
                g_writes++;
                g_dirty_forced_writes++;
            }

            e->valid = 0;
            e->dirty = 0;
            e->access_count = 0;  /* reset on eviction */
            g_evictions++;
            return idx;
        }
        idx = g_lru[idx].prev;
    }

    return -1; /* all entries are pinned */
}

/* ── Core cache operations ──────────────────────────────────────────── */

/* Fill a cache entry with fresh data from disk */
static int cache_fill(int16_t idx, uint64_t lba, uint8_t dev_id) {
    struct bc_entry *e = &g_entries[idx];
    e->lba = lba;
    e->dev_id = dev_id;
    e->valid = 1;
    e->dirty = 0;
    e->access_count = 1;  /* first access */
    e->refcount = 0;      /* no outstanding references yet */

    /* Read from disk */
    if (blockdev_read_sectors(dev_id, (uint32_t)lba, 1, e->data) != 0) {
        e->valid = 0;
        return -1;
    }

    hash_insert(idx);
    lru_push_head(idx);
    g_count++;
    return 0;
}

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
        g_entries[idx].refcount++;  /* pin buffer for caller */
        /* Update working set estimate */
        if (g_entries[idx].access_count > g_ws_est)
            g_ws_est = (g_ws_est + 1) * 2;
        spinlock_irqsave_release(&g_bc_lock, irq_flags);
        return g_entries[idx].data;
    }

    g_misses++;
    g_total_accesses++;

    /* Cache miss — need to fill */
    int16_t victim;
    if (g_count < BC_CAPACITY) {
        /* Use the LRU tail (free pool entry) */
        victim = g_lru_tail;
        lru_remove(victim);
    } else {
        /* Evict an existing entry */
        victim = evict_one();
        if (victim < 0) {
            spinlock_irqsave_release(&g_bc_lock, irq_flags);
            return NULL;
        }
        lru_remove(victim);
    }

    if (cache_fill(victim, lba, dev_id) < 0) {
        /* Fill failed — put entry back in free pool */
        lru_push_head(victim);
        g_entries[victim].valid = 0;
        spinlock_irqsave_release(&g_bc_lock, irq_flags);
        return NULL;
    }

    spinlock_irqsave_release(&g_bc_lock, irq_flags);
    return g_entries[victim].data;
}

/* Release a previously acquired buffer cache entry.
 * Decrements the refcount, allowing the entry to be evicted later. */
void bufcache_release(uint64_t lba, uint8_t dev_id) {
    if (!g_active || !g_initialized) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    int16_t idx = hash_lookup(lba, dev_id);
    if (idx >= 0 && g_entries[idx].refcount > 0) {
        g_entries[idx].refcount--;
    }

    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

int bufcache_mark_dirty(uint64_t lba, uint8_t dev_id) {
    if (!g_active || !g_initialized) return -1;

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
        return blockdev_write_sectors(dev_id, (uint32_t)lba, 1, data);
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

    /* Cache miss — fill then update */
    int16_t victim;
    if (g_count < BC_CAPACITY) {
        victim = g_lru_tail;
        lru_remove(victim);
    } else {
        victim = evict_one();
        if (victim < 0) {
            /* Cache full with dirty entries — write directly */
            spinlock_irqsave_release(&g_bc_lock, irq_flags);
            return blockdev_write_sectors(dev_id, (uint32_t)lba, 1, data);
        }
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
    g_count++;

    spinlock_irqsave_release(&g_bc_lock, irq_flags);

    /* Throttle: flush if too many dirty buffers on this device */
    bufcache_throttle_writes(dev_id);
    return 0;
}

void bufcache_flush(void) {
    if (!g_initialized) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    for (int i = 0; i < BC_CAPACITY; i++) {
        if (g_entries[i].valid && g_entries[i].dirty) {
            blockdev_write_sectors(g_entries[i].dev_id,
                                    (uint32_t)g_entries[i].lba, 1,
                                    g_entries[i].data);
            g_writes++;
            g_entries[i].dirty = 0;
        }
    }

    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

void bufcache_flush_all(void) {
    bufcache_flush();
}

/* ── Flush dirty entries for a specific device ──────────────────────── */

void bufcache_flush_dev(uint8_t dev_id) {
    if (!g_initialized) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    for (int i = 0; i < BC_CAPACITY; i++) {
        if (g_entries[i].valid && g_entries[i].dirty && g_entries[i].dev_id == dev_id) {
            blockdev_write_sectors(g_entries[i].dev_id,
                                    (uint32_t)g_entries[i].lba, 1,
                                    g_entries[i].data);
            g_writes++;
            g_entries[i].dirty = 0;
        }
    }

    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

/* ── Writeback: flush dirty pages without invalidating ──────────────── */

int bufcache_writeback(void) {
    if (!g_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    int written = 0;
    /* Walk the LRU list from tail (coldest) to head (hottest) */
    int16_t idx = g_lru_tail;
    while (idx >= 0) {
        if (g_entries[idx].valid && g_entries[idx].dirty) {
            blockdev_write_sectors(g_entries[idx].dev_id,
                                    (uint32_t)g_entries[idx].lba, 1,
                                    g_entries[idx].data);
            g_entries[idx].dirty = 0;
            g_writes++;
            written++;
        }
        idx = g_lru[idx].prev;
    }

    spinlock_irqsave_release(&g_bc_lock, irq_flags);
    return written;
}

void bufcache_set_dirty(uint64_t lba, uint8_t dev_id) {
    if (!g_active || !g_initialized) return;
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
    if (!g_active || !g_initialized) return;
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);
    int16_t idx = hash_lookup(lba, dev_id);
    if (idx >= 0) {
        g_entries[idx].dirty = 0;
    }
    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

void bufcache_invalidate(uint64_t lba, uint8_t dev_id) {
    if (!g_active || !g_initialized) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);

    int16_t idx = hash_lookup(lba, dev_id);
    if (idx >= 0) {
        struct bc_entry *e = &g_entries[idx];
        if (e->dirty) {
            blockdev_write_sectors(dev_id, (uint32_t)lba, 1, e->data);
            g_writes++;
            e->dirty = 0;
        }
        hash_remove(idx);
        e->valid = 0;
        lru_remove(idx);
        /* Return to free pool at tail */
        g_lru[idx].prev = g_lru_tail;
        g_lru[idx].next = -1;
        if (g_lru_tail >= 0) g_lru[g_lru_tail].next = (int16_t)idx;
        g_lru_tail = (int16_t)idx;
        if (g_lru_head < 0) g_lru_head = (int16_t)idx;
        g_count--;
    }

    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

/* Enhanced stats */
void bufcache_stats_ex(uint64_t *total_accesses, uint64_t *evictions,
                        uint64_t *dirty_forced_writes, uint32_t *ws_est) {
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_bc_lock, &irq_flags);
    if (total_accesses)       *total_accesses       = g_total_accesses;
    if (evictions)            *evictions            = g_evictions;
    if (dirty_forced_writes)  *dirty_forced_writes  = g_dirty_forced_writes;
    if (ws_est)               *ws_est               = g_ws_est;
    spinlock_irqsave_release(&g_bc_lock, irq_flags);
}

/* ── bufcache_read ─────────────────────────────────────── */
int bufcache_read(void *buf, size_t count, uint64_t block)
{
    (void)buf;
    (void)count;
    (void)block;
    kprintf("[bufcache] bufcache_read\n");
    return 0;
}
