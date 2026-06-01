#ifndef KASAN_LIGHT_H
#define KASAN_LIGHT_H

#include "types.h"

/* Enable/disable KASAN checking at runtime */
#define KASAN_ENABLED 1

/* Shadow memory granule: 1 byte of shadow per 8 bytes of heap */
#define KASAN_GRANULE_SIZE 8

/* Shadow values */
#define KASAN_SHADOW_FREE   0xFF  /* inaccessible (freed or poisoned) */
#define KASAN_SHADOW_ACCESS 0x00  /* accessible (allocated or unpoisoned) */
#define KASAN_SHADOW_REDZONE 0xFE /* redzone padding (out-of-bounds) */

/* Initialize shadow memory region for the kernel heap.
 * Must be called after heap_init(). */
void kasan_init(void);

/* Mark a memory region as poisoned (inaccessible).
 * addr must be KASAN_GRANULE_SIZE-aligned. */
void kasan_poison(const void *addr, size_t size);

/* Mark a memory region as accessible. */
void kasan_unpoison(const void *addr, size_t size);

/* Check if a memory region is accessible.
 * Returns 0 if OK (all bytes accessible), -EFAULT if poisoned. */
int kasan_check(const void *addr, size_t size, int is_write);

/* Convenience: unpoison after kmalloc (call from allocator hook). */
void kasan_alloc(const void *addr, size_t size);

/* Convenience: poison after kfree (call from allocator hook). */
void kasan_free(const void *addr, size_t size);

#endif /* KASAN_LIGHT_H */
