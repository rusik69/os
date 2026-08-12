#include "heap.h"

#include "export.h"
#include "fault_inject.h"
#include "kasan_light.h"
#include "kmemleak.h"
#include "pmm.h"
#include "spinlock.h"
#include "string.h"

/*
 * ────────────────────────────────────────────────────────────────────────────
 * Heap — Kernel Dynamic Memory Allocator
 * ────────────────────────────────────────────────────────────────────────────
 *
 * OVERVIEW
 * --------
 * The kernel heap provides dynamic memory allocation for kernel code via
 * kmalloc(), kfree(), krealloc(), and kcalloc().  It uses a first-fit
 * free-list algorithm with immediate coalescing of adjacent free blocks.
 *
 * The heap lives in the high-half VMA region (KERNEL_VMA_OFFSET offset).
 * Boot code maps the first 1 GB via 2 MB huge pages so no VMM page-table
 * calls are needed for the initial heap.  Physical frames are reserved
 * incrementally in PMM as the heap grows (via pmm_reserve_frames), ensuring
 * that VMM page-table allocations cannot accidentally steal heap pages.
 *
 * MEMORY LAYOUT
 * ─────────────
 *   heap_base       — Start of heap region (right above kernel .bss end,
 *                     aligned to PAGE_SIZE)
 *   heap_current    — Current allocation pointer (top of used area)
 *   heap_limit      — End of currently reserved physical frames
 *   heap_base_phys  — Physical address corresponding to heap_base
 *   HEAP_MAX_SIZE   — Maximum heap size: 64 MB
 *   HEAP_INITIAL    — Initial reservation: 4 pages (16 KB)
 *
 * The heap expands on demand: when a first-fit scan finds no suitable free
 * block, heap_expand() reserves additional physical frames and extends
 * heap_limit.  If the expansion would exceed HEAP_MAX_SIZE, ENOMEM is
 * returned.
 *
 * BLOCK STRUCTURE  (struct heap_block)
 * ─────────────────
 * Each allocation is preceded by a block header:
 *
 *   ┌─────────────────────────────┐
 *   │ struct heap_block           │ ← 40 bytes overhead per allocation
 *   │   magic  (8 bytes) — canary │     (rounded to 48 with alignment)
 *   │   size   (8 bytes)          │
 *   │   free   (4 bytes)          │
 *   │   next   (8 bytes) — ptr    │
 *   │   prev   (8 bytes) — ptr    │
 *   ├─────────────────────────────┤
 *   │ caller's data               │ ← returned by kmalloc
 *   │   ...                       │
 *   └─────────────────────────────┘
 *
 *   magic = HEAP_BLOCK_MAGIC (0xE1E0E3E2E5E4E7E6) — detects heap metadata
 *           corruption via buffer overflows or wild pointers.
 *   size  = usable bytes for the caller (not including header).
 *   free  = 1 if the block is free, 0 if allocated.
 *   next  = pointer to next block in the doubly linked list.
 *   prev  = pointer to previous block in the doubly linked list.
 *
 * ALLOCATION  (kmalloc, line ~148)
 * ────────────
 *   1. Validate size (non-zero, overflow-safe for alignment).
 *   2. Align to 16 bytes (required for x86-64 SSE/AVX compatibility).
 *   3. Acquire heap_lock (spinlock with IRQ save).
 *   4. First-fit search: scan the free list from heap_start_block.
 *      - If the block is large enough and has room to split (> header + 16),
 *        split into allocated + remainder free block.
 *      - Mark as used, update heap_used_bytes, call KASAN/ kmemleak hooks.
 *   5. If no fit found, call heap_expand() to grow the heap.
 *      - Check for SIZE_MAX overflow before expanding.
 *      - Create a new block at heap_current, advance pointer.
 *   6. Return pointer to the data portion (header + 16).
 *
 * DEALLOCATION  (kfree, line ~241)
 * ──────────────
 *   1. NULL check (kfree(NULL) is a safe no-op).
 *   2. Acquire heap_lock.
 *   3. Get block header from (ptr - BLOCK_HDR_SIZE).
 *   4. Validate magic canary — if corrupted, log a critical warning
 *      but continue freeing (least-worst option to avoid leaks).
 *   5. Call KASAN free hook (poison the freed region).
 *   6. Call kmemleak free hook.
 *   7. Mark block as free.
 *   8. Forward coalesce: if the next block is free, merge them.
 *   9. Backward coalesce: if the previous block is free, merge them.
 *
 * REALLOCATION  (krealloc, line ~255)
 * ───────────────
 *   1. NULL ptr → kmalloc(new_size).
 *   2. Zero new_size → kfree(ptr), return NULL.
 *   3. If existing block is large enough, return ptr as-is.
 *   4. Otherwise allocate new block, memcpy old data, free old block.
 *
 * CANARY / CORRUPTION DETECTION
 * ──────────────────────────────
 * Each heap block starts with a magic value (HEAP_BLOCK_MAGIC).  This is
 * verified on every kfree and krealloc.  A mismatch indicates heap metadata
 * corruption (typically a buffer overflow from the previous allocation).
 * The kernel logs a critical message but attempts to continue, avoiding
 * a double-free or memory leak.
 *
 * DEBUGGING & INTEGRITY
 * ──────────────────────
 *   heap_check()   — Walks the entire free list, validating block sizes,
 *                    magic canaries, prev/next pointer consistency, and
 *                    detecting adjacent-free-block violations.
 *   heap_stats()   — Returns total/used/free bytes and block counts.
 *   KASAN          — Marks allocated regions accessible, freed regions
 *                    poisoned to catch use-after-free.
 *   kmemleak       — Tracks every allocation by caller IP for memory leak
 *                    detection.
 *   fault injection — kmalloc can optionally fail to test OOM paths
 *                     (fault_inject_should_fail_kmalloc).
 *
 * THREAD SAFETY
 * ──────────────
 * All heap operations are protected by a single spinlock (heap_lock) with
 * IRQ save/restore, making them safe from both process context and interrupt
 * handlers.  The internal _kmalloc_locked / _kfree_locked helpers exist so
 * that krealloc can call both without recursive lock deadlock.
 * ────────────────────────────────────────────────────────────────────────────
 */
#include "heap.h"
#define HEAP_MAX_SIZE (64ULL * 1024 * 1024) /* 64 MB — cc needs ~7 MB per compile */
#define HEAP_INITIAL (4ULL * 4096)          /* 4 pages */

/* Dedicated heap region base (physical).  The heap MUST NOT sit right above
 * _kernel_end: that region is where the PMM hot cache / get_or_create_table
 * allocate page-table frames (phys 0x4000000-0x4007000) and where the kernel
 * stacks live.  The heap grew UP through those frames and kmalloc handed the
 * kernel .text PT (phys 0x4002000) out as a file buffer — FAT reads then
 * clobbered the page tables (infinite #PF fetch loop in page_fault_handler).
 * 0x4200000 (66 MB) is above every pre-heap_init allocation and is
 * identity-mapped by the physmap huge pages (pd[0x21]+). */
#define HEAP_PHYS_BASE 0x4200000ULL

#define HEAP_BLOCK_MAGIC 0xE1E0E3E2E5E4E7E6ULL /* canary — detects heap metadata corruption */

struct heap_block {
    uint64_t magic; /* must be HEAP_BLOCK_MAGIC — corruption canary */
    size_t size;
    int free;
    struct heap_block *next;
    struct heap_block *prev;
};

#define BLOCK_HDR_SIZE sizeof(struct heap_block)

static struct heap_block *heap_start_block = NULL;
static uint64_t heap_base = 0;
static uint64_t heap_current = 0;
static uint64_t heap_limit = 0;
static uint64_t heap_base_phys = 0;  /* physical address of heap base */
static uint64_t heap_used_bytes = 0; /* running total of bytes in use */
static spinlock_t heap_lock;         /* protects all shared state above */

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
    /* Dedicated heap region at HEAP_PHYS_BASE — see the define above for why
     * it must not sit right above the kernel image. */
    heap_base = (uint64_t)PHYS_TO_VIRT(HEAP_PHYS_BASE);
    heap_current = heap_base + HEAP_INITIAL;
    heap_limit = heap_current;
    heap_base_phys = VIRT_TO_PHYS(heap_base);

    /* Reserve the ENTIRE heap range up front so the PMM never hands these
     * frames to page tables / kernel stacks / disk buffers.  The previous
     * lazy per-expansion reservation was unsafe: pmm_reserve_frames()
     * silently skips frames that are already allocated, so when the heap
     * grew into a range the PMM had already given to get_or_create_table,
     * the expansion "succeeded" and kmalloc reused those page-table frames
     * as data (FAT reads clobbered the kernel .text PT → #PF fetch loop). */
    pmm_reserve_frames(heap_base_phys, HEAP_MAX_SIZE);

    /* Advance PMM alloc hint past the reserved heap region so scanning
     * allocations (user pages, stacks, page tables) come from above it. */
    pmm_advance_hint(heap_base_phys + HEAP_MAX_SIZE);

    /* Set up initial free block (high-half VMA — mapped via PML4[256] huge pages) */
    heap_start_block = (struct heap_block *)heap_base;
    heap_start_block->magic = HEAP_BLOCK_MAGIC;
    heap_start_block->size = HEAP_INITIAL - BLOCK_HDR_SIZE;
    heap_start_block->free = 1;
    heap_start_block->next = NULL;
    heap_start_block->prev = NULL;

    spinlock_init(&heap_lock);
}

/*
 * Internal locked helpers for kmalloc/kfree — caller must hold heap_lock.
 * These are used by krealloc to avoid recursive spinlock deadlock.
 */

static void *_kmalloc_locked(size_t size) {
    /* size must already be aligned and > 0 */

    /* First fit */
    struct heap_block *block = heap_start_block;
    while (block) {
        if (block->free && block->size >= size) {
            /* Split if possible */
            if (block->size > size + BLOCK_HDR_SIZE + 16) {
                struct heap_block *new_block =
                    (struct heap_block *)((uint8_t *)block + BLOCK_HDR_SIZE + size);
                new_block->magic = HEAP_BLOCK_MAGIC;
                new_block->size = block->size - size - BLOCK_HDR_SIZE;
                new_block->free = 1;
                new_block->next = block->next;
                new_block->prev = block;
                if (new_block->next)
                    new_block->next->prev = new_block;
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
        if (!block->next)
            break;
        block = block->next;
    }

    /* No free block found, expand heap */
    /* Check for overflow: size + block_header must not wrap */
    if (size > SIZE_MAX - BLOCK_HDR_SIZE)
        return NULL;
    size_t total = size + BLOCK_HDR_SIZE;
    if (heap_expand(total) < 0)
        return NULL;

    struct heap_block *new_block = (struct heap_block *)heap_current;
    heap_current += total;
    new_block->magic = HEAP_BLOCK_MAGIC;
    new_block->size = size;
    new_block->free = 0;
    new_block->next = NULL;
    new_block->prev = block;

    if (block)
        block->next = new_block;
    else
        heap_start_block = new_block;

    heap_used_bytes += total;
    void *ptr = (void *)((uint8_t *)new_block + BLOCK_HDR_SIZE);
    /* KASAN: mark the newly allocated region as accessible */
    kasan_alloc(ptr, new_block->size);
    /* kmemleak: track this allocation */
    kmemleak_alloc(ptr, new_block->size, KMEMLEAK_HEAP);
    return ptr;
}

void *__malloc kmalloc(size_t size) {
    if (size == 0)
        return NULL;
    if (heap_base == 0)
        return NULL;

    /* Fault injection: if enabled, fail this allocation to test error paths */
    if (fault_inject_should_fail_kmalloc()) {
        return NULL;
    }

    /* Validate allocation size to prevent overflow during alignment.
     * If size is too close to SIZE_MAX, the (size + 15) alignment step
     * would wrap, producing a small or zero allocation that does not
     * match the caller's intended buffer size. */
    if (size > SIZE_MAX - 16)
        return NULL;

    /* Align to 16 bytes */
    size = (size + 15) & ~15ULL;

    uint64_t flags;
    spinlock_irqsave_acquire(&heap_lock, &flags);
    void *ret = _kmalloc_locked(size);
    spinlock_irqsave_release(&heap_lock, flags);
    return ret;
}

uint64_t heap_get_total(void) {
    return HEAP_MAX_SIZE;
}

uint64_t heap_get_used(void) {
    uint64_t flags;
    spinlock_irqsave_acquire(&heap_lock, &flags);
    uint64_t used = heap_used_bytes;
    spinlock_irqsave_release(&heap_lock, flags);
    return used;
}

uint64_t heap_get_free(void) {
    uint64_t flags;
    spinlock_irqsave_acquire(&heap_lock, &flags);
    uint64_t used = heap_used_bytes;
    spinlock_irqsave_release(&heap_lock, flags);
    if (used >= HEAP_MAX_SIZE)
        return 0;
    return HEAP_MAX_SIZE - used;
}

/*
 * Internal locked helper — caller must hold heap_lock.
 * Called by kfree and krealloc.
 */
static void _kfree_locked(void *ptr) {
    struct heap_block *block = (struct heap_block *)((uint8_t *)ptr - BLOCK_HDR_SIZE);

    /* Verify heap block canary — detects buffer overflows and corruption */
    if (block->magic != HEAP_BLOCK_MAGIC) {
        kprintf("[heap] CRITICAL: heap corruption detected in kfree(%p) — "
                "block %p magic mismatch (expected 0x%016llx, actual 0x%016llx)\n",
                ptr, (void *)block, (unsigned long long)HEAP_BLOCK_MAGIC,
                (unsigned long long)block->magic);
        /* Continue with the free to avoid leaking memory — the corruption
         * may have already damaged the allocator state, but freeing the
         * block is the least-worst option. */
    }

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
        if (old_next)
            old_next->prev = block;
    }

    /* Backward coalesce with previous block */
    if (block->prev && block->prev->free) {
        struct heap_block *prev = block->prev;
        prev->size += BLOCK_HDR_SIZE + block->size;
        prev->next = block->next;
        if (block->next)
            block->next->prev = prev;
    }
}

void kfree(void *ptr) {
    if (!ptr)
        return;
    uint64_t flags;
    spinlock_irqsave_acquire(&heap_lock, &flags);
    _kfree_locked(ptr);
    spinlock_irqsave_release(&heap_lock, flags);
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

    /* Acquire the heap lock and check whether the existing block is large enough */
    uint64_t flags;
    spinlock_irqsave_acquire(&heap_lock, &flags);

    /* Get the original block header (lock held protects header from modification) */
    struct heap_block *block = (struct heap_block *)((uint8_t *)ptr - BLOCK_HDR_SIZE);

    /* Verify heap block canary before operating on the block */
    if (block->magic != HEAP_BLOCK_MAGIC) {
        kprintf("[heap] CRITICAL: heap corruption detected in krealloc(%p) — "
                "block %p magic mismatch (expected 0x%016llx, actual 0x%016llx)\n",
                ptr, (void *)block, (unsigned long long)HEAP_BLOCK_MAGIC,
                (unsigned long long)block->magic);
        spinlock_irqsave_release(&heap_lock, flags);
        return NULL;
    }

    size_t old_size = block->size;

    /* If new size fits in the existing block, return ptr as-is */
    if (new_size <= old_size) {
        spinlock_irqsave_release(&heap_lock, flags);
        return ptr;
    }

    /* Validate new_size to prevent overflow during alignment */
    if (new_size > SIZE_MAX - 16) {
        spinlock_irqsave_release(&heap_lock, flags);
        return NULL;
    }

    /* Align size for the new allocation */
    size_t aligned_size = (new_size + 15) & ~15ULL;

    /* Allocate a new block using the internal locked helper (lock already held) */
    void *new_ptr = _kmalloc_locked(aligned_size);
    if (!new_ptr) {
        spinlock_irqsave_release(&heap_lock, flags);
        return NULL;
    }

    /* Copy old data (aligned_size > old_size in this branch, so min is old_size) */
    memcpy(new_ptr, ptr, old_size);

    /* Free old block using internal locked helper (lock still held) */
    _kfree_locked(ptr);

    spinlock_irqsave_release(&heap_lock, flags);
    return new_ptr;
}

/* ── kcalloc — zero-initialised array allocation ─────────────────── */

void *__malloc kcalloc(size_t nmemb, size_t size) {
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
int heap_stats(void *stats) {
    if (!stats)
        return -EINVAL;
    /* Fill a heap_stat structure */
    struct {
        uint64_t total_size;
        uint64_t used_bytes;
        uint64_t free_bytes;
        uint64_t block_count;
        uint64_t free_block_count;
    } st;

    uint64_t flags;
    spinlock_irqsave_acquire(&heap_lock, &flags);

    st.total_size = HEAP_MAX_SIZE;
    st.used_bytes = heap_used_bytes;
    st.free_bytes = (heap_used_bytes >= HEAP_MAX_SIZE) ? 0 : HEAP_MAX_SIZE - heap_used_bytes;

    /* Count blocks */
    st.block_count = 0;
    st.free_block_count = 0;
    struct heap_block *b = heap_start_block;
    while (b) {
        st.block_count++;
        if (b->free)
            st.free_block_count++;
        b = b->next;
    }

    spinlock_irqsave_release(&heap_lock, flags);

    memcpy(stats, &st, sizeof(st));
    return 0;
}

/* ── heap_check ─────────────────────────────── */
static int heap_check(void) {
    uint64_t flags;
    spinlock_irqsave_acquire(&heap_lock, &flags);

    struct heap_block *b = heap_start_block;
    int errors = 0;

    while (b) {
        /* Validate block header sanity */
        if (b->size == 0 || b->size > HEAP_MAX_SIZE) {
            kprintf("[heap] heap_check: ERROR block %p has invalid size %llu\n", (void *)b,
                    (unsigned long long)b->size);
            errors++;
        }
        /* Validate block magic (canary) — detects heap metadata corruption */
        if (b->magic != HEAP_BLOCK_MAGIC) {
            kprintf("[heap] heap_check: ERROR block %p has corrupted magic "
                    "(expected 0x%016llx, actual 0x%016llx)\n",
                    (void *)b, (unsigned long long)HEAP_BLOCK_MAGIC, (unsigned long long)b->magic);
            errors++;
        }
        /* Validate prev/next consistency */
        if (b->next && b->next->prev != b) {
            kprintf("[heap] heap_check: ERROR block %p: next->prev mismatch\n", (void *)b);
            errors++;
        }
        if (b->prev && b->prev->next != b) {
            kprintf("[heap] heap_check: ERROR block %p: prev->next mismatch\n", (void *)b);
            errors++;
        }
        /* Adjacent free blocks should have been coalesced */
        if (b->free && b->next && b->next->free) {
            kprintf("[heap] heap_check: ERROR adjacent free blocks at %p and %p\n", (void *)b,
                    (void *)b->next);
            errors++;
        }
        b = b->next;
    }

    if (errors == 0)
        kprintf("[heap] heap_check: OK (%d blocks, %llu bytes used)\n", errors,
                (unsigned long long)heap_used_bytes);
    else
        kprintf("[heap] heap_check: %d ERRORS found\n", errors);

    spinlock_irqsave_release(&heap_lock, flags);
    return errors;
}
