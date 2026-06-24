/*
 * stdlib_user.c — Userspace heap allocator (freelist-based)
 *
 * Implements malloc/free/calloc/realloc with a free-list manager that
 * reduces kernel syscall overhead by sub-allocating from large pools.
 *
 * Strategy:
 *   - First-fit free-list traversal.
 *   - Coalescing of adjacent free blocks on free().
 *   - Splitting of large free blocks on allocation.
 *   - Initial heap pool (64 KB) allocated via libc_malloc; expanded in
 *     64 KB chunks as needed.
 *   - Each heap block carries a header with size + in-use flag.
 *
 * Thread safety: a simple spinlock (libc_mutex) serializes heap operations.
 * This is adequate for the kernel-shell environment where preemption
 * is controlled and contention is low.
 *
 * This file implements Item U14 of the userspace infrastructure plan.
 */

#include "stdlib.h"
#include "string.h"
#include "libc.h"

/* ── Configuration ──────────────────────────────────────────────────── */

/* Default pool size and growth increment */
#define POOL_SIZE_DEFAULT   (64UL * 1024)        /* 64 KB initial */
#define POOL_GROW_SIZE      (64UL * 1024)        /* 64 KB growth chunks */

/* Minimum alignment for all allocations (16 bytes = cache-line friendly) */
#define ALIGNMENT           16UL
#define ALIGN_UP(sz)        (((sz) + ALIGNMENT - 1) & ~(ALIGNMENT - 1))

/* Minimum block size (header + minimum payload) */
#define MIN_BLOCK           32UL

/* ── Block header ───────────────────────────────────────────────────── */

/*
 * Each allocated or free block has this header immediately before the
 * user-visible pointer.  The header is 16 bytes (two uint64_t fields)
 * and holds size (including header) and flags.
 *
 * Memory layout:
 *   [header 16B] [user data ...]
 *   ^            ^
 *   header       pointer returned to caller
 *
 * Free blocks additionally store next/prev pointers in the first 16 bytes
 * of the user data area (no extra memory overhead).
 */
struct heap_header {
    uint64_t size;       /* total block size including header (low bits = flags) */
    uint64_t magic;      /* magic + owner debug field */
};

/* Flag bits stored in the low bits of ->size */
#define BLK_FLAG_INUSE   0x1UL
#define BLK_FLAG_MASK    0xFUL

/* Magic constants for corruption detection */
#define MAGIC_ALLOC      0xA110CA7E5A110CAAULL   /* "allocated" pattern */
#define MAGIC_FREE       0xCAFEBABEDEADBEEFULL   /* "freelist" pattern */

/* Extract real size from header (mask off low flag bits) */
#define BLK_SIZE(hdr)       ((hdr)->size & ~BLK_FLAG_MASK)
#define BLK_IS_INUSE(hdr)   ((hdr)->size & BLK_FLAG_INUSE)
#define BLK_SET_INUSE(hdr)  ((hdr)->size |= BLK_FLAG_INUSE)
#define BLK_CLR_INUSE(hdr)  ((hdr)->size &= ~BLK_FLAG_INUSE)

/* Next block header (walk forward by size) */
#define NEXT_HDR(hdr)       ((struct heap_header *)((char *)(hdr) + BLK_SIZE(hdr)))
#define PREV_HDR(hdr, sz)   ((struct heap_header *)((char *)(hdr) - (sz)))

/* User pointer from header */
#define HDR_TO_USER(hdr)    ((void *)((char *)(hdr) + sizeof(struct heap_header)))
#define USER_TO_HDR(ptr)    ((struct heap_header *)((char *)(ptr) - sizeof(struct heap_header)))

/* Free-list node (overlays user data area of free blocks) */
struct free_node {
    struct free_node *next;
    struct free_node *prev;
};

/* ── State ──────────────────────────────────────────────────────────── */

static struct heap_header *heap_base = (void *)0;   /* first block in heap */
static struct heap_header *heap_end  = (void *)0;   /* one past last valid byte */
static int    heap_initialized = 0;

/* Free-list head (sorted by address for easy coalescing) */
static struct free_node  free_sentinel;
static struct free_node *free_list;        /* &free_sentinel */

/* Mutex for thread safety */
static int heap_mutex = -1;

/* Statistics */
static uint64_t stat_allocs    = 0;
static uint64_t stat_frees     = 0;
static uint64_t stat_syscalls  = 0;  /* calls to kernel for new memory */

/* ── Forward declarations ───────────────────────────────────────────── */

static void  heap_local_init(void);
static void *heap_extend(uint64_t min_size);
static void  heap_coalesce(struct heap_header *hdr);
static void  freelist_add(struct heap_header *hdr);
static void  freelist_remove(struct free_node *node);

/* ── Mutex helpers ──────────────────────────────────────────────────── */

static inline void heap_lock(void) {
    if (heap_mutex >= 0) libc_mutex_lock(heap_mutex);
}

static inline void heap_unlock(void) {
    if (heap_mutex >= 0) libc_mutex_unlock(heap_mutex);
}

/* ── Initialization ─────────────────────────────────────────────────── */

static void heap_local_init(void) {
    if (heap_initialized) return;

    /* Initialize sentinel node (circular list) */
    free_sentinel.next = &free_sentinel;
    free_sentinel.prev = &free_sentinel;
    free_list = &free_sentinel;

    /* Create mutex */
    heap_mutex = libc_mutex_init();
    if (heap_mutex < 0) heap_mutex = -1;  /* fall back to no locking */

    /* Allocate initial pool */
    void *pool = libc_malloc(POOL_SIZE_DEFAULT);
    if (!pool) {
        /* Cannot allocate even the initial pool — heap is non-functional.
         * This is a fatal condition but we silently degrade: all subsequent
         * mallocs return NULL. */
        heap_initialized = 1;
        return;
    }
    stat_syscalls++;

    /* Initialize the single free block covering the entire pool */
    struct heap_header *hdr = (struct heap_header *)pool;
    hdr->size   = POOL_SIZE_DEFAULT;  /* inuse = 0 */
    hdr->magic  = MAGIC_FREE;

    heap_base = hdr;
    heap_end  = (struct heap_header *)((char *)pool + POOL_SIZE_DEFAULT);

    /* Add to free list */
    freelist_add(hdr);

    heap_initialized = 1;
}

/* ── Expand the heap by allocating a new pool from the kernel ──────── */

static void *heap_extend(uint64_t min_size) {
    uint64_t grow = (min_size > POOL_GROW_SIZE) ? min_size : POOL_GROW_SIZE;
    grow = ALIGN_UP(grow);

    void *pool = libc_malloc(grow);
    if (!pool) return (void *)0;
    stat_syscalls++;

    /* Create a free block from this new pool */
    struct heap_header *hdr = (struct heap_header *)pool;
    hdr->size  = grow;
    hdr->magic = MAGIC_FREE;

    /* Try to coalesce with the previous heap_end if adjacent */
    if (heap_end && (char *)heap_end == (char *)pool) {
        /* Adjacent — merge by extending the last block */
        /* Walk to the last block and extend it */
        struct heap_header *last = heap_base;
        if (last) {
            while ((char *)last + BLK_SIZE(last) < (char *)heap_end) {
                last = NEXT_HDR(last);
            }
            /* Check if last is free — if so, extend it instead of adding new */
            if (!BLK_IS_INUSE(last)) {
                /* Remove old last from freelist, extend, re-add */
                freelist_remove((struct free_node *)HDR_TO_USER(last));
                last->size = (uint64_t)((char *)hdr + grow - (char *)last);
                memset(HDR_TO_USER(last), 0,
                       (size_t)((char *)last + BLK_SIZE(last) - (char *)HDR_TO_USER(last)));
                freelist_add(last);
                heap_end = (struct heap_header *)((char *)pool + grow);
                return pool;
            }
        }
    }

    /* Not adjacent — just add to free list */
    freelist_add(hdr);

    /* Update heap_end if this extends beyond current */
    struct heap_header *candidate_end = (struct heap_header *)((char *)pool + grow);
    if (!heap_end || (char *)candidate_end > (char *)heap_end) {
        heap_end = candidate_end;
    }
    if (!heap_base) heap_base = hdr;

    return pool;
}

/* ── Free-list operations ───────────────────────────────────────────── */

/* Add a free block to the freelist, sorted by address for coalescing */
static void freelist_add(struct heap_header *hdr) {
    struct free_node *node = (struct free_node *)HDR_TO_USER(hdr);

    /* Find insertion point (address-sorted) */
    struct free_node *pos = free_list->next;
    while (pos != free_list && (char *)pos < (char *)node) {
        pos = pos->next;
    }

    /* Insert before pos */
    node->next = pos;
    node->prev = pos->prev;
    pos->prev->next = node;
    pos->prev = node;

    hdr->magic = MAGIC_FREE;
}

static void freelist_remove(struct free_node *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    /* Poison removed node */
    node->next = (struct free_node *)0xdead000000000001ULL;
    node->prev = (struct free_node *)0xdead000000000002ULL;
}

/* Coalesce a block with its next neighbor if both are free */
static void heap_coalesce(struct heap_header *hdr) {
    if (BLK_IS_INUSE(hdr)) return;

    /* Try merging with next block */
    struct heap_header *next = NEXT_HDR(hdr);
    while ((char *)next < (char *)heap_end && !BLK_IS_INUSE(next) && next->magic == MAGIC_FREE) {
        /* Remove next from freelist */
        freelist_remove((struct free_node *)HDR_TO_USER(next));
        /* Extend current block */
        hdr->size += BLK_SIZE(next);
        hdr->magic = MAGIC_FREE;
        /* Check the new next */
        next = NEXT_HDR(hdr);
    }

    /* Try merging with previous — scan from base to find preceding block */
    if (heap_base && (char *)hdr > (char *)heap_base) {
        /* Walk to find the block just before hdr */
        struct heap_header *prev = heap_base;
        while ((char *)NEXT_HDR(prev) < (char *)hdr) {
            prev = NEXT_HDR(prev);
        }
        if ((char *)NEXT_HDR(prev) == (char *)hdr && !BLK_IS_INUSE(prev) && prev->magic == MAGIC_FREE) {
            /* Remove prev from freelist, extend backward */
            freelist_remove((struct free_node *)HDR_TO_USER(prev));
            prev->size += BLK_SIZE(hdr);
            prev->magic = MAGIC_FREE;
            /* Also remove hdr from freelist if it was there */
            if (!BLK_IS_INUSE(hdr)) {
                freelist_remove((struct free_node *)HDR_TO_USER(hdr));
            }
            /* Now try to coalesce the merged block forward */
            heap_coalesce(prev);
        }
    }
}

/* ── Core alloc/free ────────────────────────────────────────────────── */

void * __malloc malloc(size_t size) {
    if (size == 0) size = 1;

    /* Align requested size and add header overhead */
    uint64_t need = ALIGN_UP((uint64_t)size + sizeof(struct heap_header));
    if (need < MIN_BLOCK) need = MIN_BLOCK;

    heap_lock();

    if (!heap_initialized) heap_local_init();
    if (!heap_base) {
        heap_unlock();
        return (void *)0;
    }

    /* First-fit search */
    struct free_node *node = free_list->next;
    while (node != free_list) {
        struct heap_header *hdr = (struct heap_header *)((char *)node - sizeof(struct heap_header));

        /* Sanity check */
        if (hdr->magic != MAGIC_FREE) {
            /* Corruption detected — skip this block */
            node = node->next;
            continue;
        }

        uint64_t blk_sz = BLK_SIZE(hdr);

        if (blk_sz >= need) {
            /* Found a suitable block — split if remainder is large enough */
            uint64_t remaining = blk_sz - need;

            /* Remove this block from freelist */
            freelist_remove(node);

            if (remaining >= MIN_BLOCK) {
                /* Split: allocate the first 'need' bytes, leave remaining as free */
                hdr->size  = need | BLK_FLAG_INUSE;
                hdr->magic = MAGIC_ALLOC;

                /* Create free block from remainder */
                struct heap_header *free_hdr = NEXT_HDR(hdr);
                free_hdr->size  = remaining;
                free_hdr->magic = MAGIC_FREE;
                freelist_add(free_hdr);
            } else {
                /* Use whole block */
                hdr->size  = blk_sz | BLK_FLAG_INUSE;
                hdr->magic = MAGIC_ALLOC;
            }

            stat_allocs++;
            heap_unlock();
            return HDR_TO_USER(hdr);
        }

        node = node->next;
    }

    /* No suitable free block — extend the heap */
    uint64_t grow_size = (need > POOL_GROW_SIZE) ? need : POOL_GROW_SIZE;
    grow_size = ALIGN_UP(grow_size);

    void *pool = heap_extend(grow_size);
    if (!pool) {
        heap_unlock();
        return (void *)0;   /* ENOMEM */
    }

    /* Allocate from the new pool (it's the last entry in freelist) */
    struct heap_header *hdr = (struct heap_header *)pool;
    uint64_t blk_sz = BLK_SIZE(hdr);

    freelist_remove((struct free_node *)HDR_TO_USER(hdr));

    if (blk_sz >= need + MIN_BLOCK) {
        hdr->size  = need | BLK_FLAG_INUSE;
        hdr->magic = MAGIC_ALLOC;

        struct heap_header *free_hdr = NEXT_HDR(hdr);
        free_hdr->size  = blk_sz - need;
        free_hdr->magic = MAGIC_FREE;
        freelist_add(free_hdr);
    } else {
        hdr->size  = blk_sz | BLK_FLAG_INUSE;
        hdr->magic = MAGIC_ALLOC;
    }

    stat_allocs++;
    heap_unlock();
    return HDR_TO_USER(hdr);
}

void free(void *ptr) {
    if (!ptr) return;

    heap_lock();

    struct heap_header *hdr = USER_TO_HDR(ptr);

    /* Corruption / double-free check */
    if (!BLK_IS_INUSE(hdr)) {
        /* Double free — silently ignore in production */
        heap_unlock();
        return;
    }
    if (hdr->magic != MAGIC_ALLOC) {
        /* Corrupted header — skip to avoid worse damage */
        heap_unlock();
        return;
    }

    /* Mark as free */
    BLK_CLR_INUSE(hdr);
    hdr->magic = MAGIC_FREE;

    /* Clear user data to catch use-after-free (poison with 0x6b) */
    memset(ptr, 0x6b, (size_t)(BLK_SIZE(hdr) - sizeof(struct heap_header)));

    /* Add to freelist */
    freelist_add(hdr);

    /* Try coalescing */
    heap_coalesce(hdr);

    stat_frees++;
    heap_unlock();
}

void * __malloc calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (nmemb && size && total / nmemb != size) return (void *)0;  /* overflow */

    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) {
        free(ptr);
        return malloc(1);   /* per POSIX, free and return unique pointer */
    }

    heap_lock();

    struct heap_header *hdr = USER_TO_HDR(ptr);

    /* Corruption check */
    if (!BLK_IS_INUSE(hdr) || hdr->magic != MAGIC_ALLOC) {
        heap_unlock();
        return (void *)0;
    }

    uint64_t old_size = BLK_SIZE(hdr) - sizeof(struct heap_header);
    uint64_t need = ALIGN_UP(new_size + sizeof(struct heap_header));
    if (need < MIN_BLOCK) need = MIN_BLOCK;

    /* If the current block is large enough, just return */
    if (BLK_SIZE(hdr) >= need) {
        /* Optional: shrink the block if significantly larger */
        uint64_t excess = BLK_SIZE(hdr) - need;
        if (excess >= MIN_BLOCK) {
            /* Shrink current block */
            hdr->size = need | BLK_FLAG_INUSE;
            /* Create new free block from excess */
            struct heap_header *free_hdr = NEXT_HDR(hdr);
            free_hdr->size  = excess;
            free_hdr->magic = MAGIC_FREE;
            freelist_add(free_hdr);
            heap_coalesce(free_hdr);
        }
        heap_unlock();
        return ptr;
    }

    /* Check if next block is free and large enough to merge */
    struct heap_header *next = NEXT_HDR(hdr);
    if ((char *)next < (char *)heap_end && !BLK_IS_INUSE(next) && next->magic == MAGIC_FREE) {
        uint64_t combined = BLK_SIZE(hdr) + BLK_SIZE(next);
        if (combined >= need) {
            /* Merge with next free block */
            freelist_remove((struct free_node *)HDR_TO_USER(next));
            hdr->size = combined | BLK_FLAG_INUSE;
            hdr->magic = MAGIC_ALLOC;

            uint64_t excess = combined - need;
            if (excess >= MIN_BLOCK) {
                hdr->size = need | BLK_FLAG_INUSE;
                struct heap_header *new_free = NEXT_HDR(hdr);
                new_free->size  = excess;
                new_free->magic = MAGIC_FREE;
                freelist_add(new_free);
                heap_coalesce(new_free);
            }

            heap_unlock();
            return ptr;
        }
    }

    heap_unlock();

    /* Fall back: allocate new block, copy data, free old */
    void *new_ptr = malloc(new_size);
    if (!new_ptr) return (void *)0;

    uint64_t copy_sz = (new_size < old_size) ? new_size : old_size;
    memcpy(new_ptr, ptr, (size_t)copy_sz);
    free(ptr);
    return new_ptr;
}

/* ── strtok_r — reentrant tokenizer ──────────────────────────────────── */
char *strtok_r(char *str, const char *delim, char **saveptr)
{
    if (!delim || !saveptr)
        return NULL;
    if (str == NULL)
        str = *saveptr;
    if (str == NULL || *str == '\0') {
        *saveptr = str;
        return NULL;
    }

    /* Skip leading delimiters */
    str += strspn(str, delim);
    if (*str == '\0') {
        *saveptr = str;
        return NULL;
    }

    /* Find end of token */
    char *end = str;
    while (*end && !strchr(delim, *end))
        end++;

    if (*end == '\0') {
        *saveptr = end;
    } else {
        *end = '\0';
        *saveptr = end + 1;
    }

    return str;
}

/* ── strdup — heap allocate a copy of string ─────────────────────────── */
char *strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len + 1);
    return p;
}

/* ── strndup — heap allocate a copy of up to n characters ────────────── */
char *strndup(const char *s, size_t n)
{
    if (!s) return NULL;
    size_t slen = strlen(s);
    if (slen > n) slen = n;
    char *p = (char *)malloc(slen + 1);
    if (!p) return NULL;
    memcpy(p, s, slen);
    p[slen] = '\0';
    return p;
}

/* ── Statistics / Debugging ─────────────────────────────────────────── */

void heap_stats(uint64_t *out_allocs, uint64_t *out_frees,
                uint64_t *out_syscalls, uint64_t *out_free_bytes) {
    heap_lock();
    if (out_allocs)    *out_allocs    = stat_allocs;
    if (out_frees)     *out_frees     = stat_frees;
    if (out_syscalls)  *out_syscalls  = stat_syscalls;

    if (out_free_bytes) {
        uint64_t free_bytes = 0;
        struct free_node *node = free_list->next;
        while (node != free_list) {
            struct heap_header *hdr = (struct heap_header *)((char *)node - sizeof(struct heap_header));
            if (hdr->magic == MAGIC_FREE) {
                free_bytes += BLK_SIZE(hdr);
            }
            node = node->next;
        }
        *out_free_bytes = free_bytes;
    }
    heap_unlock();
}

/* ── malloc_user ─────────────────────────────── */
void* malloc_user(size_t size)
{
    return malloc(size);
}
/* ── free_user ─────────────────────────────── */
int free_user(void *ptr)
{
    if (ptr) free(ptr);
    return 0;
}
/* ── realloc_user ─────────────────────────────── */
void* realloc_user(void *ptr, size_t size)
{
    return realloc(ptr, size);
}
