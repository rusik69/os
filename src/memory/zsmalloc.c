// SPDX-License-Identifier: GPL-2.0-only
/*
 * zsmalloc.c — Zsmalloc allocator (for zram)
 *
 * Implements a compact slab-like allocator for storing compressed
 * pages. Designed for zram to store many small compressed objects
 * efficiently with low fragmentation.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "pmm.h"
#include "heap.h"

#define ZSMALLOC_MAX_POOLS    4
#define ZSMALLOC_PAGE_ORDER   0  /* 4KB pages */
#define ZSMALLOC_OBJ_SIZE_MAX 2048

/* Zs mapmode for mapping objects */
enum zs_mapmode {
    ZS_MM_RO,  /* read-only */
    ZS_MM_WO,  /* write-only */
    ZS_MM_RW   /* read-write */
};

/* Size classes for zsmalloc */
#define ZSMALLOC_SIZE_CLASSES 16

struct zsmalloc_size_class {
    int size;
    int objs_per_page;
};

/* Zsmalloc page descriptor */
struct zsmalloc_page {
    uint8_t *page;
    uint16_t *obj_table; /* object offset table */
    int nr_free;
    int nr_objs;
    int in_use;
};

struct zsmalloc_pool {
    struct zsmalloc_size_class classes[ZSMALLOC_SIZE_CLASSES];
    struct zsmalloc_page *pages[256];
    int nr_pages;
    int class_count;
    spinlock_t lock;
};

static struct zsmalloc_pool zsmalloc_pools[ZSMALLOC_MAX_POOLS];
static int zsmalloc_pool_count;

/* Size classes: 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048 */
static const int zsmalloc_class_sizes[ZSMALLOC_SIZE_CLASSES] = {
    32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 0, 0, 0
};

/* Find the size class index for a given size */
static int zsmalloc_find_class(struct zsmalloc_pool *pool, size_t size)
{
    for (int i = 0; i < pool->class_count; i++) {
        if (pool->classes[i].size >= (int)size)
            return i;
    }
    return -ENOMEM;
}

/* Create a new zsmalloc pool */
int zsmalloc_create_pool(void)
{
    if (zsmalloc_pool_count >= ZSMALLOC_MAX_POOLS)
        return -ENOMEM;

    struct zsmalloc_pool *pool = &zsmalloc_pools[zsmalloc_pool_count];
    memset(pool, 0, sizeof(*pool));
    spinlock_init(&pool->lock);

    /* Initialize size classes */
    pool->class_count = 0;
    for (int i = 0; i < ZSMALLOC_SIZE_CLASSES; i++) {
        if (zsmalloc_class_sizes[i] == 0) break;
        pool->classes[pool->class_count].size = zsmalloc_class_sizes[i];
        pool->classes[pool->class_count].objs_per_page =
            4096 / zsmalloc_class_sizes[i];
        pool->class_count++;
    }

    zsmalloc_pool_count++;
    return zsmalloc_pool_count - 1;
}

/* Allocate an object from zsmalloc pool */
void *zsmalloc_alloc(int pool_id, size_t size)
{
    if (pool_id < 0 || pool_id >= zsmalloc_pool_count)
        return NULL;

    struct zsmalloc_pool *pool = &zsmalloc_pools[pool_id];
    uint64_t irq_flags;

    spinlock_irqsave_acquire(&pool->lock, &irq_flags);

    int class_idx = zsmalloc_find_class(pool, size);
    if (class_idx < 0) {
        spinlock_irqsave_release(&pool->lock, irq_flags);
        return NULL;
    }

    /* Find a page with free objects in this size class */
    for (int p = 0; p < pool->nr_pages; p++) {
        struct zsmalloc_page *zp = pool->pages[p];
        if (zp && zp->nr_free > 0 && zp->nr_objs >= 1) {
            /* Find a free object in this page */
            for (int o = 0; o < zp->nr_objs; o++) {
                if (zp->obj_table && (zp->obj_table[o] == 0)) {
                    zp->obj_table[o] = 1;
                    zp->nr_free--;
                    void *obj = zp->page + (size_t)o * pool->classes[class_idx].size;
                    spinlock_irqsave_release(&pool->lock, irq_flags);
                    return obj;
                }
            }
        }
    }

    /* Allocate a new page */
    uint64_t frame = pmm_alloc_frame();
    if (!frame) {
        spinlock_irqsave_release(&pool->lock, irq_flags);
        return NULL;
    }

    struct zsmalloc_page *zp = (struct zsmalloc_page *)kmalloc(sizeof(struct zsmalloc_page));
    if (!zp) {
        pmm_free_frame(frame);
        spinlock_irqsave_release(&pool->lock, irq_flags);
        return NULL;
    }

    zp->page = (uint8_t *)PHYS_TO_VIRT(frame << 12);
    memset(zp->page, 0, 4096);

    zp->nr_objs = pool->classes[class_idx].objs_per_page;
    zp->obj_table = (uint16_t *)kmalloc(
        (size_t)zp->nr_objs * sizeof(uint16_t));
    if (!zp->obj_table) {
        pmm_free_frame(frame);
        kfree(zp);
        spinlock_irqsave_release(&pool->lock, irq_flags);
        return NULL;
    }
    memset(zp->obj_table, 0, (size_t)zp->nr_objs * sizeof(uint16_t));

    /* Mark first object as used */
    zp->obj_table[0] = 1;
    zp->nr_free = zp->nr_objs - 1;
    zp->in_use = 1;

    if (pool->nr_pages < 256)
        pool->pages[pool->nr_pages++] = zp;

    void *obj = zp->page;
    spinlock_irqsave_release(&pool->lock, irq_flags);
    return obj;
}

/* Free an object back to the pool */
void zsmalloc_free(int pool_id, void *obj)
{
    if (pool_id < 0 || pool_id >= zsmalloc_pool_count || !obj)
        return;

    struct zsmalloc_pool *pool = &zsmalloc_pools[pool_id];
    uint64_t irq_flags;

    spinlock_irqsave_acquire(&pool->lock, &irq_flags);

    /* Find which page this object belongs to */
    for (int p = 0; p < pool->nr_pages; p++) {
        struct zsmalloc_page *zp = pool->pages[p];
        if (!zp || !zp->page) continue;

        uintptr_t obj_addr = (uintptr_t)obj;
        uintptr_t page_start = (uintptr_t)zp->page;
        uintptr_t page_end = page_start + 4096;

        if (obj_addr >= page_start && obj_addr < page_end) {
            int obj_idx = (int)((obj_addr - page_start) /
                                (4096 / zp->nr_objs));
            if (obj_idx >= 0 && obj_idx < zp->nr_objs && zp->obj_table) {
                if (zp->obj_table[obj_idx]) {
                    zp->obj_table[obj_idx] = 0;
                    zp->nr_free++;
                }
            }
            break;
        }
    }

    spinlock_irqsave_release(&pool->lock, irq_flags);
}

void zsmalloc_init(void)
{
    memset(zsmalloc_pools, 0, sizeof(zsmalloc_pools));
    zsmalloc_pool_count = 0;
    kprintf("[OK] Zsmalloc — Compact compressed page allocator (for zram)\n");
}
#include "module.h"
module_init(zsmalloc_init);

/* ── zs_create_pool — Create a zsmalloc pool ─────────────── */
void* zs_create_pool(const char *name)
{
    if (zsmalloc_pool_count >= ZSMALLOC_MAX_POOLS)
        return NULL;

    struct zsmalloc_pool *pool = &zsmalloc_pools[zsmalloc_pool_count];
    memset(pool, 0, sizeof(struct zsmalloc_pool));
    spinlock_init(&pool->lock);

    /* Set up size classes for common compressed object sizes */
    int sizes[] = { 16, 32, 64, 128, 256, 384, 512, 768, 1024, 1536, 2048 };
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    if (num_sizes > ZSMALLOC_SIZE_CLASSES)
        num_sizes = ZSMALLOC_SIZE_CLASSES;

    for (int i = 0; i < num_sizes; i++) {
        pool->classes[i].size = sizes[i];
        if (sizes[i] == 0) continue;
        pool->classes[i].objs_per_page = PAGE_SIZE / sizes[i];
        if (pool->classes[i].objs_per_page < 1)
            pool->classes[i].objs_per_page = 1;
    }
    pool->class_count = num_sizes;
    pool->nr_pages = 0;

    zsmalloc_pool_count++;

    kprintf("[zsmalloc] zs_create_pool: '%s' created with %d size classes\n",
            name ? name : "unnamed", num_sizes);
    return (void *)pool;
}

/* ── zs_destroy_pool — Destroy a zsmalloc pool ─────────────── */
int zs_destroy_pool(void *pool)
{
    if (!pool)
        return -EINVAL;

    struct zsmalloc_pool *p = (struct zsmalloc_pool *)pool;

    /* Free all pages allocated by this pool */
    for (int i = 0; i < p->nr_pages; i++) {
        if (p->pages[i]) {
            struct zsmalloc_page *zp = p->pages[i];
            if (zp->page) {
                pmm_free_frame((uint64_t)(uintptr_t)zp->page);
                zp->page = NULL;
            }
            if (zp->obj_table) {
                kfree(zp->obj_table);
                zp->obj_table = NULL;
            }
            kfree(zp);
            p->pages[i] = NULL;
        }
    }
    p->nr_pages = 0;

    kprintf("[zsmalloc] zs_destroy_pool: pool destroyed\n");
    return 0;
}
/* ── zs_map_object ───────────────────────────────────────────── */
void* zs_map_object(void *pool, void *handle, enum zs_mapmode mm)
{
    (void)pool;
    (void)handle;
    (void)mm;
    /* Return the kernel virtual address for the compressed object.
     * The handle points to the object offset within the zspage. */
    kprintf("[zsmalloc] zs_map_object: mapping object\n");
    return handle;
}

/* ── zs_unmap_object ─────────────────────────────────────────── */
void zs_unmap_object(void *pool, void *handle)
{
    (void)pool;
    (void)handle;
    /* Unmap the object — in this simple implementation, a no-op.
     * A real implementation would flush caches / TLB if needed. */
    kprintf("[zsmalloc] zs_unmap_object: unmapping object\n");
}
void* zs_malloc(void *pool, size_t size)
{
    if (!pool || size == 0 || size > 2048) {
        kprintf("[zsmalloc] zs_malloc: invalid args (size=%zu)\n", size);
        return NULL;
    }
    int pool_id = -1;
    for (int i = 0; i < zsmalloc_pool_count; i++) {
        if (&zsmalloc_pools[i] == (struct zsmalloc_pool *)pool) {
            pool_id = i;
            break;
        }
    }
    if (pool_id < 0) return NULL;
    void *obj = zsmalloc_alloc(pool_id, size);
    if (obj) kprintf("[zsmalloc] zs_malloc: alloc'd %zu bytes\n", size);
    return obj;
}
/* ── Stub: zs_free ─────────────────────────────── */
int zs_free(void *pool, void *ptr)
{
    if (!pool || !ptr)
        return -EINVAL;
    int pool_id = -1;
    for (int i = 0; i < zsmalloc_pool_count; i++) {
        if (&zsmalloc_pools[i] == (struct zsmalloc_pool *)pool) {
            pool_id = i;
            break;
        }
    }
    if (pool_id < 0) return -EINVAL;
    zsmalloc_free(pool_id, ptr);
    kprintf("[zsmalloc] zs_free: freed\n");
    return 0;
}
