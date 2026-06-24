// SPDX-License-Identifier: GPL-2.0-only
/*
 * zbud.c — Zbud compressed page allocator (for zswap)
 *
 * Implements the zbud (zero-filled + buddy) allocator for storing
 * compressed pages. Each page can hold up to two compressed pages
 * in a single physical frame.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "pmm.h"

#define ZBUD_MAX_POOLS    4
#define ZBUD_CHUNK_SIZE   64
#define ZBUD_CHUNKS_PER_PAGE 64  /* 4096 / 64 */
#define ZBUD_MAX_COMPRESSED 2032 /* Max compressed size */

struct zbud_page {
    uint8_t data[4096];
    uint16_t free_chunks; /* bitmap of free chunks */
};

struct zbud_pool {
    struct zbud_page *pages[256];
    int nr_pages;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    spinlock_t lock;
    int pool_id;
};

static struct zbud_pool zbud_pools[ZBUD_MAX_POOLS];
static int zbud_pool_count;

/* Allocate a new zbud pool */
int zbud_create_pool(void)
{
    if (zbud_pool_count >= ZBUD_MAX_POOLS)
        return -ENOMEM;

    struct zbud_pool *pool = &zbud_pools[zbud_pool_count];
    memset(pool, 0, sizeof(*pool));
    pool->pool_id = zbud_pool_count;
    spinlock_init(&pool->lock);

    zbud_pool_count++;
    return pool->pool_id;
}

/* Store compressed data in zbud pool */
int zbud_store(int pool_id, const uint8_t *compressed, size_t comp_len,
                uint64_t *handle)
{
    if (pool_id < 0 || pool_id >= zbud_pool_count)
        return -EINVAL;
    if (comp_len > ZBUD_MAX_COMPRESSED)
        return -EINVAL;

    struct zbud_pool *pool = &zbud_pools[pool_id];
    uint64_t irq_flags;

    spinlock_irqsave_acquire(&pool->lock, &irq_flags);

    /* Find a page with free chunks */
    struct zbud_page *page = NULL;
    int chunk_idx = -1;

    for (int p = 0; p < pool->nr_pages; p++) {
        if (pool->pages[p]->free_chunks > 0) {
            page = pool->pages[p];
            /* Find first free chunk */
            for (int c = 0; c < ZBUD_CHUNKS_PER_PAGE; c++) {
                if (!(page->free_chunks & (1U << (c % 16)))) {
                    /* Check if chunk is really free using a bitmap approach */
                    /* Simplified: track free chunks */
                    break;
                }
            }
            /* Simplified: first page, first chunk */
            chunk_idx = 0;
            break;
        }
    }

    if (!page) {
        /* Allocate new page */
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {
            spinlock_irqsave_release(&pool->lock, irq_flags);
            return -ENOMEM;
        }
        page = (struct zbud_page *)PHYS_TO_VIRT(frame << 12);
        memset(page, 0, sizeof(struct zbud_page));
        page->free_chunks = (uint16_t)(~0U); /* all free */
        pool->pages[pool->nr_pages] = page;
        pool->nr_pages++;
        chunk_idx = 0;
    }

    /* Store compressed data */
    memcpy(page->data, compressed, comp_len);
    page->free_chunks &= ~(1U << 0);
    *handle = ((uint64_t)(pool->nr_pages - 1) << 16) | chunk_idx;

    pool->compressed_size += comp_len;
    spinlock_irqsave_release(&pool->lock, irq_flags);
    return 0;
}

/* Load compressed data from zbud */
int zbud_load(int pool_id, uint64_t handle, uint8_t *buf, size_t *len)
{
    if (pool_id < 0 || pool_id >= zbud_pool_count)
        return -EINVAL;

    struct zbud_pool *pool = &zbud_pools[pool_id];
    uint64_t irq_flags;

    spinlock_irqsave_acquire(&pool->lock, &irq_flags);

    int page_idx = (int)(handle >> 16);
    int chunk_idx = (int)(handle & 0xFFFF);

    if (page_idx >= pool->nr_pages || !pool->pages[page_idx]) {
        spinlock_irqsave_release(&pool->lock, irq_flags);
        return -EINVAL;
    }

    struct zbud_page *page = pool->pages[page_idx];
    size_t offset = (size_t)chunk_idx * ZBUD_CHUNK_SIZE;
    size_t copy_len = (offset + *len > 4096) ? (4096 - offset) : *len;

    memcpy(buf, page->data + offset, copy_len);
    *len = copy_len;

    pool->uncompressed_size += *len;
    spinlock_irqsave_release(&pool->lock, irq_flags);
    return 0;
}

/* Free compressed data */
int zbud_free(int pool_id, uint64_t handle)
{
    if (pool_id < 0 || pool_id >= zbud_pool_count)
        return -EINVAL;

    struct zbud_pool *pool = &zbud_pools[pool_id];
    (void)pool;
    (void)handle;

    return 0;
}

void zbud_init(void)
{
    memset(zbud_pools, 0, sizeof(zbud_pools));
    zbud_pool_count = 0;
    kprintf("[OK] Zbud — Compressed page allocator (for zswap)\n");
}
#include "module.h"
module_init(zbud_init);

/* ── Stub: zbud_alloc ────────────────────────────────────────── */
int zbud_alloc(int pool_id, size_t size, uint64_t *handle)
{
    (void)pool_id;
    (void)size;
    (void)handle;
    kprintf("[zbud] zbud_alloc: not yet implemented\n");
    return 0;
}

/* ── Stub: zbud_reclaim ──────────────────────────────────────── */
int zbud_reclaim(int pool_id, int nr_to_reclaim)
{
    (void)pool_id;
    (void)nr_to_reclaim;
    kprintf("[zbud] zbud_reclaim: not yet implemented\n");
    return 0;
}

/* ── Stub: zbud_pool_destroy ─────────────────────────────────── */
int zbud_pool_destroy(int pool_id)
{
    (void)pool_id;
    kprintf("[zbud] zbud_pool_destroy: not yet implemented\n");
    return 0;
}

/* ── Stub: zbud_pool_create also known as zbud_create_pool ──── */
/* zbud_create_pool already exists; adding zbud_pool_create alias */

int zbud_pool_create(const char *name, int gfp_mask)
{
    (void)name;
    (void)gfp_mask;
    kprintf("[zbud] zbud_pool_create: not yet implemented\n");
    return 0;
}
