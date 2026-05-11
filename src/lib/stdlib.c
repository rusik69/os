#include "stdlib.h"
#include "string.h"
#include "heap.h"

/* ---- Internal byte-swap helper ---- */
static void swap_bytes(char *a, char *b, size_t sz) {
    while (sz--) { char t = *a; *a++ = *b; *b++ = t; }
}

/*
 * qsort — Lomuto-partition quicksort with insertion sort for small subarrays.
 * Pivot is chosen as the middle element to avoid O(n²) on sorted input.
 */
void qsort(void *base, size_t n, size_t sz,
           int (*cmp)(const void *, const void *)) {
    char *b = (char *)base;

    /* Insertion sort for small arrays */
    if (n <= 16) {
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j > 0 && cmp(b + (j-1)*sz, b + j*sz) > 0; j--)
                swap_bytes(b + (j-1)*sz, b + j*sz, sz);
        return;
    }

    /* Move median-of-three pivot to last position */
    size_t mid = n / 2;
    if (cmp(b, b + mid*sz) > 0)     swap_bytes(b,          b + mid*sz,    sz);
    if (cmp(b, b + (n-1)*sz) > 0)   swap_bytes(b,          b + (n-1)*sz,  sz);
    if (cmp(b + mid*sz, b + (n-1)*sz) > 0)
                                     swap_bytes(b + mid*sz, b + (n-1)*sz,  sz);
    /* pivot is now at b[(n-1)*sz]; Lomuto partition */
    char *pivot = b + (n-1)*sz;
    size_t i = 0;
    for (size_t j = 0; j < n - 1; j++) {
        if (cmp(b + j*sz, pivot) <= 0) {
            swap_bytes(b + i*sz, b + j*sz, sz);
            i++;
        }
    }
    swap_bytes(b + i*sz, pivot, sz);

    if (i > 0)       qsort(b,            i,       sz, cmp);
    if (i + 1 < n)   qsort(b + (i+1)*sz, n-i-1,   sz, cmp);
}

/*
 * bsearch — standard binary search; returns pointer to matching element or NULL.
 */
void *bsearch(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *)) {
    const char *lo = (const char *)base;
    const char *hi = lo + n * sz;
    while (lo < hi) {
        const char *mid = lo + ((size_t)(hi - lo) / sz / 2) * sz;
        int r = cmp(key, mid);
        if (r == 0) return (void *)mid;
        if (r < 0)  hi = mid;
        else        lo = mid + sz;
    }
    return (void *)0;
}

/*
 * strdup — heap-allocate a copy of s.  Caller must kfree() the result.
 */
char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = (char *)kmalloc(len);
    if (p) memcpy(p, s, len);
    return p;
}
