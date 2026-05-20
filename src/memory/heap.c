#include "heap.h"
#include "pmm.h"
#include "string.h"

/*
 * Heap lives in the identity-mapped 0-1GB region (boot code maps it via 2MB huge
 * pages so no vmm_map_page calls are needed).  We place it right above the kernel
 * binary and pre-reserve the region in PMM so VMM page-table allocations cannot
 * steal those frames.
 */

#define HEAP_MAX_SIZE (12ULL * 1024 * 1024)   /* 12 MB */
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

static void heap_expand(size_t needed) {
    uint64_t new_limit = heap_current + needed;
    if (new_limit > heap_base + HEAP_MAX_SIZE)
        return;   /* exhausted */
    heap_limit = new_limit;
}

void heap_init(void) {
    /* Align heap base to PAGE_SIZE above the kernel binary */
    heap_base    = ((uint64_t)_kernel_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    heap_current = heap_base + HEAP_INITIAL;
    heap_limit   = heap_current;

    /* Reserve the entire heap region in PMM so nobody else allocates those frames */
    pmm_reserve_frames(heap_base, HEAP_MAX_SIZE);

    /* Set up initial free block (no vmm_map_page needed – identity-mapped) */
    heap_start_block        = (struct heap_block *)heap_base;
    heap_start_block->size  = HEAP_INITIAL - BLOCK_HDR_SIZE;
    heap_start_block->free  = 1;
    heap_start_block->next  = NULL;
    heap_start_block->prev  = NULL;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

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
            return (void *)((uint8_t *)block + BLOCK_HDR_SIZE);
        }
        if (!block->next) break;
        block = block->next;
    }

    /* No free block found, expand heap */
    size_t total = size + BLOCK_HDR_SIZE;
    heap_expand(total);

    struct heap_block *new_block = (struct heap_block *)heap_current;
    heap_current += total;
    new_block->size = size;
    new_block->free = 0;
    new_block->next = NULL;
    new_block->prev = block;

    if (block) block->next = new_block;
    else heap_start_block = new_block;

    return (void *)((uint8_t *)new_block + BLOCK_HDR_SIZE);
}

static uint64_t heap_walk_used(void) {
    uint64_t used = 0;
    struct heap_block *block = heap_start_block;
    while (block) {
        if (!block->free)
            used += block->size + BLOCK_HDR_SIZE;
        block = block->next;
    }
    return used;
}

uint64_t heap_get_total(void) {
    return HEAP_MAX_SIZE;
}

uint64_t heap_get_used(void) {
    return heap_walk_used();
}

uint64_t heap_get_free(void) {
    uint64_t used = heap_walk_used();
    if (used >= HEAP_MAX_SIZE) return 0;
    return HEAP_MAX_SIZE - used;
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct heap_block *block = (struct heap_block *)((uint8_t *)ptr - BLOCK_HDR_SIZE);
    block->free = 1;

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
