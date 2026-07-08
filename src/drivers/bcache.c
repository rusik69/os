// SPDX-License-Identifier: GPL-2.0-only
/*
 * bcache.c — Block cache (SSD caching for HDD)
 *
 * Provides SSD-based caching for slow block devices (HDDs).
 * Implements cache device registration, lookup, and I/O forwarding.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "heap.h"

#define BCACHE_MAX_CACHES    8
#define BCACHE_BUCKET_SIZE   512000  /* 500KB buckets */
#define BCACHE_CACHE_SIZE    (256 * 1024 * 1024) /* 256MB cache default */

struct bcache_device {
    int active;
    char backing_dev[64];   /* HDD backing device */
    char cache_dev[64];     /* SSD cache device */
    uint8_t *cache_data;    /* Cache data buffer */
    uint64_t cache_size;    /* Cache size in bytes */
    uint64_t bucket_size;
    uint32_t *bucket_map;   /* Backing device block -> cache bucket */
    uint32_t *bucket_dirty; /* Dirty flags per bucket */
    uint64_t hits;
    uint64_t misses;
    spinlock_t lock;
};

static struct bcache_device bcache_devices[BCACHE_MAX_CACHES];
static int bcache_count = 0;

/* Register a cache device */
static int bcache_register(const char *backing, const char *cache)
{
    uint64_t irq_flags;
    int idx = -1;

    for (int i = 0; i < BCACHE_MAX_CACHES; i++) {
        if (bcache_devices[i].active) continue;
        idx = i;
        break;
    }
    if (idx < 0) return -ENOMEM;

    struct bcache_device *bc = &bcache_devices[idx];

    spinlock_irqsave_acquire(&bc->lock, &irq_flags);

    strncpy(bc->backing_dev, backing, sizeof(bc->backing_dev) - 1);
    bc->backing_dev[sizeof(bc->backing_dev) - 1] = '\0';
    strncpy(bc->cache_dev, cache, sizeof(bc->cache_dev) - 1);
    bc->cache_dev[sizeof(bc->cache_dev) - 1] = '\0';
    bc->cache_size = BCACHE_CACHE_SIZE;
    bc->bucket_size = BCACHE_BUCKET_SIZE;

    /* Allocate cache data (simplified: use kmalloc) */
    bc->cache_data = (uint8_t *)kmalloc(BCACHE_CACHE_SIZE);
    if (!bc->cache_data) {
        spinlock_irqsave_release(&bc->lock, irq_flags);
        return -ENOMEM;
    }
    memset(bc->cache_data, 0, BCACHE_CACHE_SIZE);

    int num_buckets = (int)(BCACHE_CACHE_SIZE / BCACHE_BUCKET_SIZE);
    bc->bucket_map = (uint32_t *)kmalloc((size_t)num_buckets * sizeof(uint32_t));
    bc->bucket_dirty = (uint32_t *)kmalloc((size_t)num_buckets * sizeof(uint32_t));
    if (!bc->bucket_map || !bc->bucket_dirty) {
        kfree(bc->cache_data);
        kfree(bc->bucket_map);
        kfree(bc->bucket_dirty);
        spinlock_irqsave_release(&bc->lock, irq_flags);
        return -ENOMEM;
    }

    memset(bc->bucket_map, 0xFF, (size_t)num_buckets * sizeof(uint32_t));
    memset(bc->bucket_dirty, 0, (size_t)num_buckets * sizeof(uint32_t));
    bc->active = 1;
    bcache_count++;

    spinlock_irqsave_release(&bc->lock, irq_flags);
    return 0;
}

/* Look up a block in cache */
static int bcache_lookup(int cache_id, uint64_t block_no, uint8_t *buf, size_t len)
{
    if (cache_id < 0 || cache_id >= BCACHE_MAX_CACHES)
        return -EINVAL;
    struct bcache_device *bc = &bcache_devices[cache_id];
    if (!bc->active) return -ENODEV;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&bc->lock, &irq_flags);

    int num_buckets = (int)(bc->cache_size / bc->bucket_size);
    uint32_t bucket = (uint32_t)(block_no % (uint32_t)num_buckets);

    if (bc->bucket_map[bucket] == (uint32_t)block_no) {
        /* Cache hit */
        uint64_t offset = (uint64_t)bucket * bc->bucket_size;
        if (offset + len <= bc->cache_size) {
            memcpy(buf, bc->cache_data + offset, len);
        }
        bc->hits++;
        spinlock_irqsave_release(&bc->lock, irq_flags);
        return 1; /* hit */
    }

    /* Cache miss */
    bc->misses++;
    spinlock_irqsave_release(&bc->lock, irq_flags);
    return 0; /* miss */
}

/* Insert a block into cache */
static int bcache_insert(int cache_id, uint64_t block_no,
                   const uint8_t *buf, size_t len, int dirty)
{
    if (cache_id < 0 || cache_id >= BCACHE_MAX_CACHES)
        return -EINVAL;
    struct bcache_device *bc = &bcache_devices[cache_id];
    if (!bc->active) return -ENODEV;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&bc->lock, &irq_flags);

    int num_buckets = (int)(bc->cache_size / bc->bucket_size);
    uint32_t bucket = (uint32_t)(block_no % (uint32_t)num_buckets);
    uint64_t offset = (uint64_t)bucket * bc->bucket_size;

    if (offset + len <= bc->cache_size) {
        memcpy(bc->cache_data + offset, buf, len);
        bc->bucket_map[bucket] = (uint32_t)block_no;
        bc->bucket_dirty[bucket] = dirty ? 1 : 0;
    }

    spinlock_irqsave_release(&bc->lock, irq_flags);
    return 0;
}

/* Get cache statistics */
static void bcache_get_stats(int cache_id, uint64_t *hits, uint64_t *misses)
{
    if (cache_id < 0 || cache_id >= BCACHE_MAX_CACHES)
        return;
    struct bcache_device *bc = &bcache_devices[cache_id];
    if (!bc->active) return;

    if (hits) *hits = bc->hits;
    if (misses) *misses = bc->misses;
}

static void bcache_init(void)
{
    memset(bcache_devices, 0, sizeof(bcache_devices));
    for (int i = 0; i < BCACHE_MAX_CACHES; i++)
        spinlock_init(&bcache_devices[i].lock);
    kprintf("[OK] BCache — SSD block cache for HDDs (%d max, %llu MB cache)\n",
            BCACHE_MAX_CACHES,
            (unsigned long long)(BCACHE_CACHE_SIZE / (1024 * 1024)));
}
#include "module.h"
module_init(bcache_init);

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: bcache_read ─────────────────────────────── */
static int bcache_read(struct bcache_device *dev, uint64_t sector, void *buf, uint32_t count)
{
    (void)dev;
    (void)sector;
    (void)buf;
    (void)count;
    kprintf("[BCACHE] bcache_read: not yet implemented\n");
    return 0;
}
/* ── Stub: bcache_write ────────────────────────────── */
static int bcache_write(struct bcache_device *dev, uint64_t sector, const void *buf, uint32_t count)
{
    (void)dev;
    (void)sector;
    (void)buf;
    (void)count;
    kprintf("[BCACHE] bcache_write: not yet implemented\n");
    return 0;
}
/* ── Stub: bcache_open ─────────────────────────────── */
static int bcache_open(struct bcache_device *dev)
{
    (void)dev;
    kprintf("[BCACHE] bcache_open: not yet implemented\n");
    return 0;
}
/* ── Stub: bcache_close ────────────────────────────── */
static void bcache_close(struct bcache_device *dev)
{
    (void)dev;
    kprintf("[BCACHE] bcache_close: not yet implemented\n");
}
/* ── Stub: bcache_flush ────────────────────────────── */
static int bcache_flush(struct bcache_device *dev)
{
    (void)dev;
    kprintf("[BCACHE] bcache_flush: not yet implemented\n");
    return 0;
}
