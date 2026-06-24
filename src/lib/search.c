#include "search.h"
#include "string.h"    /* memcpy */

/* ---- Internal byte-swap helper ---- */
static void swap_bytes(char *a, char *b, size_t sz) {
    while (sz--) { char t = *a; *a++ = *b; *b++ = t; }
}

/*
 * qsort — Lomuto-partition quicksort with insertion sort fallback for small
 * subarrays and a recursion depth limit (2*log2(n)) to guarantee O(n log n)
 * worst-case stack depth.
 */
#define QSORT_DEPTH_MAX 64

static void qsort_insertion(char *b, size_t n, size_t sz,
                             int (*cmp)(const void *, const void *)) {
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(b + (j-1)*sz, b + j*sz) > 0; j--)
            swap_bytes(b + (j-1)*sz, b + j*sz, sz);
}

static void qsort_impl(void *base, size_t n, size_t sz,
                        int (*cmp)(const void *, const void *), int depth) {
    char *b = (char *)base;

    if (n <= 16 || depth >= QSORT_DEPTH_MAX) {
        qsort_insertion(b, n, sz, cmp);
        return;
    }

    /* Median-of-three pivot selection */
    size_t mid = n / 2;
    if (cmp(b, b + mid*sz) > 0)     swap_bytes(b,          b + mid*sz,    sz);
    if (cmp(b, b + (n-1)*sz) > 0)   swap_bytes(b,          b + (n-1)*sz,  sz);
    if (cmp(b + mid*sz, b + (n-1)*sz) > 0)
                                     swap_bytes(b + mid*sz, b + (n-1)*sz,  sz);

    char *pivot = b + (n-1)*sz;
    size_t i = 0;
    for (size_t j = 0; j < n - 1; j++) {
        if (cmp(b + j*sz, pivot) <= 0) {
            swap_bytes(b + i*sz, b + j*sz, sz);
            i++;
        }
    }
    swap_bytes(b + i*sz, pivot, sz);

    if (i > 0)       qsort_impl(b,            i,       sz, cmp, depth + 1);
    if (i + 1 < n)   qsort_impl(b + (i+1)*sz, n-i-1,   sz, cmp, depth + 1);
}

void qsort_ext(void *base, size_t n, size_t sz,
           int (*cmp)(const void *, const void *)) {
    qsort_impl(base, n, sz, cmp, 0);
}

/*
 * bsearch_ext — binary search over a sorted array.
 * Returns pointer to matching element or NULL.
 */
void *bsearch_ext(const void *key, const void *base, size_t n, size_t sz,
              int (*cmp)(const void *, const void *)) {
    if (sz == 0) return NULL;
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
 * lfind — linear search over an unsorted array.
 * *nmemb is the number of elements; size is element size.
 * Returns pointer to matching element or NULL.
 */
void *lfind(const void *key, const void *base, size_t *nmemb, size_t size,
            int (*compar)(const void *, const void *)) {
    const char *p = (const char *)base;
    for (size_t i = 0; i < *nmemb; i++) {
        if (compar(key, p + i * size) == 0)
            return (void *)(p + i * size);
    }
    return (void *)0;
}

/*
 * lsearch — linear search; if key is not found, append it to the array.
 * *nmemb is incremented on insertion.
 * Returns pointer to the matching or newly inserted element.
 */
void *lsearch(const void *key, void *base, size_t *nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    char *p = (char *)base;
    for (size_t i = 0; i < *nmemb; i++) {
        if (compar(key, p + i * size) == 0)
            return (void *)(p + i * size);
    }
    /* Not found — append */
    memcpy(p + (*nmemb) * size, key, size);
    (*nmemb)++;
    return (void *)(p + (*nmemb - 1) * size);
}

/* ── search_binary ─────────────────────────────── */
static int search_binary(const void *key, const void *base, size_t nmemb, size_t size, void *cmp)
{
    if (!key || !base || !cmp)
        return -1;
    int (*compar)(const void *, const void *) = (int (*)(const void *, const void *))cmp;
    const char *lo = (const char *)base;
    const char *hi = lo + nmemb * size;
    while (lo < hi) {
        const char *mid = lo + ((size_t)(hi - lo) / size / 2) * size;
        int r = compar(key, mid);
        if (r == 0) return (int)((mid - (const char *)base) / size);
        if (r < 0)  hi = mid;
        else        lo = mid + size;
    }
    return -1;
}
/* ── search_linear ─────────────────────────────── */
static int search_linear(const void *key, const void *base, size_t nmemb, size_t size, void *cmp)
{
    if (!key || !base || !cmp)
        return -1;
    int (*compar)(const void *, const void *) = (int (*)(const void *, const void *))cmp;
    const char *p = (const char *)base;
    for (size_t i = 0; i < nmemb; i++) {
        if (compar(key, p + i * size) == 0)
            return (int)i;
    }
    return -1;
}
