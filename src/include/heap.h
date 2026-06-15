#ifndef HEAP_H
#define HEAP_H

#include "types.h"

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
void *kcalloc(size_t nmemb, size_t size);
void *krealloc(void *ptr, size_t new_size);
uint64_t heap_get_total(void);
uint64_t heap_get_used(void);
uint64_t heap_get_free(void);

/* Overflow-checking array allocation helpers.
 * Returns NULL if the multiplication would overflow SIZE_MAX. */
#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

static inline void *kmalloc_array(size_t n, size_t size)
{
    if (n && size > SIZE_MAX / n)
        return NULL;
    return kmalloc(n * size);
}

static inline void *krealloc_array(void *ptr, size_t n, size_t size)
{
    if (n && size > SIZE_MAX / n)
        return NULL;
    return krealloc(ptr, n * size);
}

static inline void *kcalloc_array(size_t n, size_t size)
{
    if (n && size > SIZE_MAX / n)
        return NULL;
    return kcalloc(n, size);
}

#endif
