#define KERNEL_INTERNAL
#include "types.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "pmm.h"
#include "heap.h"
#include "compaction.h"

/* ── Page cache: generic file data caching in memory ─────────────────── */

#define PAGE_CACHE_MAX_PAGES 1024
#define PAGE_CACHE_HASH_SIZE 64

struct page_cache_entry {
    uint64_t  ino;         /* inode number */
    uint64_t  block;       /* block index within file */
    uint64_t  phys_addr;   /* physical address of cached page */
    void     *data;        /* kernel virtual address */
    int       dirty;       /* 1 = needs writeback */
    int       in_use;
    uint64_t  last_access; /* tick of last access (for LRU) */
};

static struct page_cache_entry page_cache[PAGE_CACHE_MAX_PAGES];
static int page_cache_initialized = 0;

/* Simple hash: ino ^ block */
static int page_cache_hash(uint64_t ino, uint64_t block) {
    return (int)((ino ^ block) % PAGE_CACHE_HASH_SIZE);
}

void page_cache_init(void) {
    if (page_cache_initialized) return;
    memset(page_cache, 0, sizeof(page_cache));
    page_cache_initialized = 1;
    kprintf("[OK] page_cache initialized (%d pages)\n", PAGE_CACHE_MAX_PAGES);
}

/* Lookup a page in cache */
struct page_cache_entry *page_cache_lookup(uint64_t ino, uint64_t block) {
    if (!page_cache_initialized) return NULL;
    
    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        if (page_cache[i].in_use &&
            page_cache[i].ino == ino &&
            page_cache[i].block == block) {
            page_cache[i].last_access = 0; /* would use timer_get_ticks() */
            return &page_cache[i];
        }
    }
    return NULL;
}

/* Add a page to cache */
int page_cache_add(uint64_t ino, uint64_t block, const void *data) {
    if (!page_cache_initialized) return -1;
    
    /* Check if already exists */
    if (page_cache_lookup(ino, block)) return 0;
    
    /* Find free slot or evict LRU */
    int slot = -1;
    uint64_t oldest_access = UINT64_MAX;
    
    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        if (!page_cache[i].in_use) {
            slot = i;
            break;
        }
        if (page_cache[i].last_access < oldest_access) {
            oldest_access = page_cache[i].last_access;
            slot = i;
        }
    }
    
    if (slot < 0) return -ENOMEM;
    
    /* Evict if dirty */
    if (page_cache[slot].in_use && page_cache[slot].dirty) {
        /* Would write back here */
        kprintf("[page_cache] evicting dirty page ino=%llu block=%llu\n",
                (unsigned long long)page_cache[slot].ino,
                (unsigned long long)page_cache[slot].block);
    }
    
    /* Allocate a page */
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return -ENOMEM;
    
    void *virt = PHYS_TO_VIRT(phys);
    memcpy(virt, data, PAGE_SIZE);
    
    page_cache[slot].ino = ino;
    page_cache[slot].block = block;
    page_cache[slot].phys_addr = phys;
    page_cache[slot].data = virt;
    page_cache[slot].dirty = 0;
    page_cache[slot].in_use = 1;
    page_cache[slot].last_access = 0;
    
    return 0;
}

/* Remove a page from cache */
void page_cache_remove(uint64_t ino, uint64_t block) {
    struct page_cache_entry *pce = page_cache_lookup(ino, block);
    if (!pce) return;
    
    if (pce->phys_addr) {
        pmm_free_frame(pce->phys_addr);
    }
    memset(pce, 0, sizeof(struct page_cache_entry));
}

/* Mark page as dirty */
void page_cache_mark_dirty(uint64_t ino, uint64_t block) {
    struct page_cache_entry *pce = page_cache_lookup(ino, block);
    if (pce) pce->dirty = 1;
}

/* Flush all dirty pages */
void page_cache_flush(void) {
    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        if (page_cache[i].in_use && page_cache[i].dirty) {
            /* Would write back to backing store */
            page_cache[i].dirty = 0;
        }
    }
}
