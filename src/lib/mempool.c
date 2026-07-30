/**
 * mempool.c — Memory pool allocator
 *
 * Memory pools provide a pre-allocated cache of fixed-size elements,
 * reducing allocation overhead and memory fragmentation for frequently
 * allocated/freed objects (e.g. network buffers, inode structures, DMA
 * descriptors).
 *
 * Allocation strategy:
 *   The pool pre-allocates 'min_nr' elements at creation time, stored
 *   in a LIFO (stack) pointer array.  On mempool_alloc, an element is
 *   popped from the pool if available; otherwise a new element is
 *   allocated via kmalloc (fallback).  On mempool_free, the element is
 *   pushed back into the pool if space remains (cur_nr < max_nr);
 *   otherwise it is freed with kfree.  This hybrid approach guarantees
 *   that the pool always has at least min_nr elements available, while
 *   accommodating bursts without hard upper limits.
 *
 * max_nr sizing:
 *   max_nr = 2 * min_nr (unless min_nr is negative or exceeds the
 *   safety threshold of 0x3FFFFFFF, in which case max_nr = min_nr
 *   as a fallback to avoid overflow).
 *
 * Use cases:
 *   - Network packet descriptors (sk_buff-style structs)
 *   - Inode/dentry caches in filesystems
 *   - Block I/O request structures
 *   - Any fixed-size object with high allocation/free churn
 *
 * Thread safety:
 *   This implementation does NOT provide internal locking.  Callers
 *   must provide their own synchronization (mutex/spinlock) when the
 *   pool is accessed from multiple threads or interrupts.
 */

#include "mempool.h"
#include "heap.h"
#include "string.h"

/**
 * mempool_create - Allocate and initialise a memory pool
 * @min_nr:   Minimum number of pre-allocated elements
 * @elem_size: Size (in bytes) of each element
 *
 * Allocates the pool control structure and pre-allocates @min_nr
 * elements.  Returns NULL on allocation failure.
 */
mempool_t *mempool_create(int min_nr, int elem_size) {
    mempool_t *p = kmalloc(sizeof(mempool_t)); if (!p) return NULL;
    p->min_nr = min_nr;
    if (min_nr > 0x3FFFFFFF || min_nr < 0) {
        p->max_nr = min_nr;
    } else {
        p->max_nr = min_nr * 2;
    }
    p->elem_size = elem_size; p->cur_nr = min_nr;
    p->elements = kmalloc(sizeof(void *) * p->max_nr); if (!p->elements) { kfree(p); return NULL; }
    for (int i = 0; i < min_nr; i++) p->elements[i] = kmalloc(elem_size);
    return p;
}

/**
 * mempool_alloc - Allocate an element from the pool
 * @pool: The memory pool to allocate from
 *
 * Returns a pooled element (fast path) if one is available in the
 * pre-allocated stack, otherwise falls back to kmalloc.
 * Returns NULL only if kmalloc itself fails (the fast path never
 * fails because all pre-allocated elements are guaranteed valid).
 */
void *mempool_alloc(mempool_t *pool) { return pool->cur_nr > 0 ? pool->elements[--pool->cur_nr] : kmalloc(pool->elem_size); }

/**
 * mempool_free - Return an element to the pool
 * @e:    Element to free / return
 * @pool: The memory pool
 *
 * If the pool still has room (cur_nr < max_nr), the element is
 * pushed onto the LIFO stack for reuse; otherwise it is freed
 * via kfree to keep memory consumption bounded.
 */
void mempool_free(void *e, mempool_t *p) { if (p->cur_nr < p->max_nr) p->elements[p->cur_nr++] = e; else kfree(e); }

/**
 * mempool_destroy - Destroy a memory pool and free all elements
 * @pool: The pool to destroy
 *
 * Frees every pre-allocated element back to the slab allocator,
 * then releases the element pointer array and the pool structure
 * itself.  After this call the pool pointer is invalid.
 */
void mempool_destroy(mempool_t *p) { for (int i = 0; i < p->cur_nr; i++) kfree(p->elements[i]); kfree(p->elements); kfree(p); }
