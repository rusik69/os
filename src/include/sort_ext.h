#ifndef SORT_EXT_H
#define SORT_EXT_H

#include "types.h"

/* Generic sort with user-provided comparison and swap.
 * 'base' – pointer to the start of the array
 * 'nmemb' – number of elements
 * 'size' – size of each element in bytes
 * 'cmp' – comparison function (returns <0, 0, >0)
 * 'swap' – swap function (if NULL, uses byte-by-byte swap)
 */
void sort(void *base, size_t nmemb, size_t size,
          int (*cmp)(const void *, const void *),
          void (*swap)(void *, void *, size_t));

/* Byte-by-byte swap (the default swap backing). */
void swap_bytes(void *a, void *b, size_t size);

/* Pre-defined comparison functions */
int cmp_int(const void *a, const void *b);
int cmp_long(const void *a, const void *b);
int cmp_str(const void *a, const void *b);

void sort_ext_init(void);

#endif /* SORT_EXT_H */
