#include "heap.h"
#include "pmm.h"
#include "string.h"
#include "export.h"
#include "fault_inject.h"
#include "kasan_light.h"
#include "kmemleak.h"

/*
 * Heap lives in the high-half VMA region (boot code maps the first 1 GB via 2MB
 * huge pages so no vmm_map_page calls are needed for the heap).  We place it
 * right above the kernel binary and incrementally reserve the physical frames
 * in PMM as the heap grows, so VMM page-table allocations cannot steal heap pages.
 */

#define HEAP_MAX_SIZE (64ULL * 1024 * 1024)   /* 64 MB — cc needs ~7 MB per compile */
#define HEAP_INITIAL  (4ULL * 4096)            /* 4 pages */

struct heap_block {
    size_t size;
    int    free;
    struct heap_block *next;
    struct heap_block *prev;
};

#define BLOCK_HDR_SIZE sizeof(struct heap_block)

extern char _kernel_end[];   /* linker symbol */

static struct heap_block *heap_start_block = NULL;
static uint64_t heap_base    = 0;
static uint64_t heap_current = 0;
static uint64_t heap_limit   = 0;
static uint64_t heap_base_phys = 0; /* physical address of heap base */
static uint64_t heap_used_bytes = 0; /* running total of bytes in use */

static int heap_expand(size_t needed) {
    uint64_t new_limit = heap_current + needed;
    if (new_limit > heap_base + HEAP_MAX_SIZE)
        return -ENOMEM;

    /* Reserve the newly expanded physical frames in PMM */
    uint64_t old_limit_phys = heap_base_phys + (heap_limit - heap_base);
    uint64_t new_limit_phys = heap_base_phys + (new_limit - heap_base);
    if (new_limit_phys > old_limit_phys)
        pmm_reserve_frames(old_limit_phys, new_limit_phys - old_limit_phys);

    heap_limit = new_limit;
    return 0;
}

void __init heap_init(void) {
    /* Align heap base to PAGE_SIZE above the kernel binary */
    heap_base    = ((uint64_t)_kernel_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    heap_current = heap_base + HEAP_INITIAL;
    heap_limit   = heap_current;
    heap_base_phys = VIRT_TO_PHYS(heap_base);

    /* Reserve the initial heap pages in PMM so they are not stolen.
     * heap_base_phys is the physical address of the heap region. */
    pmm_reserve_frames(heap_base_phys, HEAP_INITIAL);

    /* Advance PMM alloc hint past the initial heap region */
    pmm_advance_hint(heap_base_phys + HEAP_INITIAL);

    /* Set up initial free block (high-half VMA — mapped via PML4[256] huge pages) */
    heap_start_block        = (struct heap_block *)heap_base;
    heap_start_block->size  = HEAP_INITIAL - BLOCK_HDR_SIZE;
    heap_start_block->free  = 1;
    heap_start_block->next  = NULL;
    heap_start_block->prev  = NULL;
}

void * __malloc kmalloc(size_t size) {
    if (size == 0) return NULL;
    if (heap_base == 0) return NULL;

    /* Fault injection: if enabled, fail this allocation to test error paths */
    if (fault_inject_should_fail_kmalloc()) {
        return NULL;
    }

    /* Align to 16 bytes */
    size = (size + 15) & ~15ULL;

    /* First fit */
    struct heap_block *block = heap_start_block;
    while (block) {
        if (block->free && block->size >= size) {
            /* Split if possible */
            if (block->size > size + BLOCK_HDR_SIZE + 16) {
                struct heap_block *new_block = (struct heap_block *)((uint8_t *)block + BLOCK_HDR_SIZE + size);
                new_block->size = block->size - size - BLOCK_HDR_SIZE;
                new_block->free = 1;
                new_block->next = block->next;
                new_block->prev = block;
                if (new_block->next) new_block->next->prev = new_block;
                block->next = new_block;
                block->size = size;
            }
            block->free = 0;
            heap_used_bytes += block->size + BLOCK_HDR_SIZE;
            void *ptr = (void *)((uint8_t *)block + BLOCK_HDR_SIZE);
            /* KASAN: mark the allocated region as accessible */
            kasan_alloc(ptr, block->size);
            /* kmemleak: track this allocation */
            kmemleak_alloc(ptr, block->size, KMEMLEAK_HEAP);
            return ptr;
        }
        if (!block->next) break;
        block = block->next;
    }

    /* No free block found, expand heap */
    size_t total = size + BLOCK_HDR_SIZE;
    if (heap_expand(total) < 0)
        return NULL;

    struct heap_block *new_block = (struct heap_block *)heap_current;
    heap_current += total;
    new_block->size = size;
    new_block->free = 0;
    new_block->next = NULL;
    new_block->prev = block;

    if (block) block->next = new_block;
    else heap_start_block = new_block;

    heap_used_bytes += total;
    void *ptr = (void *)((uint8_t *)new_block + BLOCK_HDR_SIZE);
    /* KASAN: mark the newly allocated region as accessible */
    kasan_alloc(ptr, new_block->size);
    /* kmemleak: track this allocation */
    kmemleak_alloc(ptr, new_block->size, KMEMLEAK_HEAP);
    return ptr;
}

uint64_t heap_get_total(void) {
    return HEAP_MAX_SIZE;
}

uint64_t heap_get_used(void) {
    return heap_used_bytes;
}

uint64_t heap_get_free(void) {
    uint64_t used = heap_used_bytes;
    if (used >= HEAP_MAX_SIZE) return 0;
    return HEAP_MAX_SIZE - used;
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct heap_block *block = (struct heap_block *)((uint8_t *)ptr - BLOCK_HDR_SIZE);

    /* KASAN: mark the freed region as poisoned to catch use-after-free */
    kasan_free(ptr, block->size);

    /* kmemleak: stop tracking this allocation */
    kmemleak_free(ptr);

    block->free = 1;
    heap_used_bytes -= (block->size + BLOCK_HDR_SIZE);

    /* Forward coalesce with next block */
    if (block->next && block->next->free) {
        block->size += BLOCK_HDR_SIZE + block->next->size;
        struct heap_block *old_next = block->next->next;
        block->next = old_next;
        if (old_next) old_next->prev = block;
    }

    /* Backward coalesce with previous block */
    if (block->prev && block->prev->free) {
        struct heap_block *prev = block->prev;
        prev->size += BLOCK_HDR_SIZE + block->size;
        prev->next = block->next;
        if (block->next) block->next->prev = prev;
    }
}

/* ── Exported symbols for module loading ──────────────────────────── */
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);

/* ── krealloc — resize a heap allocation ─────────────────────────── */

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr)
        return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    /* Get the original block header */
    struct heap_block *block = (struct heap_block *)((uint8_t *)ptr - BLOCK_HDR_SIZE);
    size_t old_size = block->size;

    /* If new size fits in the existing block, return ptr as-is */
    if (new_size <= old_size)
        return ptr;

    /* Otherwise allocate a new block, copy old data, free old block */
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr)
        return NULL;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    kfree(ptr);
    return new_ptr;
}

/* ── kcalloc — zero-initialised array allocation ─────────────────── */

void * __malloc kcalloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    /* Check for overflow */
    if (nmemb != 0 && total / nmemb != size)
        return NULL;
    void *ptr = kmalloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

/* ── heap_stats ─────────────────────────────── */
int heap_stats(void *stats)
{
    if (!stats) return -EINVAL;
    /* Fill a heap_stat structure */
    struct {
        uint64_t total_size;
        uint64_t used_bytes;
        uint64_t free_bytes;
        uint64_t block_count;
        uint64_t free_block_count;
    } st;

    st.total_size     = HEAP_MAX_SIZE;
    st.used_bytes     = heap_used_bytes;
    st.free_bytes     = (heap_used_bytes >= HEAP_MAX_SIZE) ? 0 : HEAP_MAX_SIZE - heap_used_bytes;

    /* Count blocks */
    st.block_count = 0;
    st.free_block_count = 0;
    struct heap_block *b = heap_start_block;
    while (b) {
        st.block_count++;
        if (b->free) st.free_block_count++;
        b = b->next;
    }

    memcpy(stats, &st, sizeof(st));
    return 0;
}

/* ── heap_check ─────────────────────────────── */
int heap_check(void)
{
    struct heap_block *b = heap_start_block;
    int errors = 0;

    while (b) {
        /* Validate block header sanity */
        if (b->size == 0 || b->size > HEAP_MAX_SIZE) {
            kprintf("[heap] heap_check: ERROR block %p has invalid size %llu\n",
                    (void *)b, (unsigned long long)b->size);
            errors++;
        }
        /* Validate prev/next consistency */
        if (b->next && b->next->prev != b) {
            kprintf("[heap] heap_check: ERROR block %p: next->prev mismatch\n",
                    (void *)b);
            errors++;
        }
        if (b->prev && b->prev->next != b) {
            kprintf("[heap] heap_check: ERROR block %p: prev->next mismatch\n",
                    (void *)b);
            errors++;
        }
        /* Adjacent free blocks should have been coalesced */
        if (b->free && b->next && b->next->free) {
            kprintf("[heap] heap_check: ERROR adjacent free blocks at %p and %p\n",
                    (void *)b, (void *)b->next);
            errors++;
        }
        b = b->next;
    }

    if (errors == 0)
        kprintf("[heap] heap_check: OK (%d blocks, %llu bytes used)\n",
                errors, (unsigned long long)heap_used_bytes);
    else
        kprintf("[heap] heap_check: %d ERRORS found\n", errors);

    return errors;
}
