#include "sort_ext.h"
#include "printf.h"
#include "string.h"
#include "kernel.h"

/*
 * Byte-by-byte swap for arbitrary-size elements.
 */
void swap_bytes(void *a, void *b, size_t size)
{
    size_t i;
    for (i = 0; i < size; i++) {
        char tmp = ((char *)a)[i];
        ((char *)a)[i] = ((char *)b)[i];
        ((char *)b)[i] = tmp;
    }
}

/* Comparison: int */
int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/* Comparison: long */
int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a;
    long y = *(const long *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/* Comparison: string (via strcmp) */
int cmp_str(const void *a, const void *b)
{
    const char *x = *(const char **)a;
    const char *y = *(const char **)b;
    return strcmp(x, y);
}

/*
 * Generic sort – insertion sort for small arrays, quicksort for larger.
 * Uses the provided cmp and swap functions.
 */
static void insertion_sort(void *base, size_t nmemb, size_t size,
                           int (*cmp)(const void *, const void *),
                           void (*swap)(void *, void *, size_t))
{
    size_t i, j;
    char *buf = (char *)base;

    for (i = 1; i < nmemb; i++) {
        j = i;
        while (j > 0 && cmp(buf + j * size, buf + (j - 1) * size) < 0) {
            swap(buf + j * size, buf + (j - 1) * size, size);
            j--;
        }
    }
}

static void qsort_helper(void *base, size_t lo, size_t hi, size_t size,
                         int (*cmp)(const void *, const void *),
                         void (*swap)(void *, void *, size_t))
{
    char *buf = (char *)base;

    while (lo < hi) {
        size_t pivot = lo + (hi - lo) / 2;
        size_t i = lo, j = hi;

        /* Use middle element as pivot */
        char *pivot_ptr = buf + pivot * size;

        while (i <= j) {
            while (cmp(buf + i * size, pivot_ptr) < 0) i++;
            while (cmp(buf + j * size, pivot_ptr) > 0) j--;
            if (i <= j) {
                swap(buf + i * size, buf + j * size, size);
                /* Update pivot if it moved */
                if (i == pivot) pivot = j;
                else if (j == pivot) pivot = i;
                pivot_ptr = buf + pivot * size;
                i++;
                j--;
            }
        }

        /* Recurse into smaller partition, loop on larger */
        if (j - lo < hi - i) {
            if (lo < j)
                qsort_helper(base, lo, j, size, cmp, swap);
            lo = i;
        } else {
            if (i < hi)
                qsort_helper(base, i, hi, size, cmp, swap);
            hi = j;
        }
    }
}

void sort(void *base, size_t nmemb, size_t size,
          int (*cmp)(const void *, const void *),
          void (*swap)(void *, void *, size_t))
{
    if (!base || nmemb < 2 || size == 0)
        return;
    if (!cmp)
        return;
    if (!swap)
        swap = swap_bytes;

    if (nmemb <= 16) {
        insertion_sort(base, nmemb, size, cmp, swap);
    } else {
        qsort_helper(base, 0, nmemb - 1, size, cmp, swap);
    }
}

void sort_ext_init(void)
{
    kprintf("[OK] sort_ext: Generic sort/swap/cmp routines initialised\n");
}

/* ── Stub: sort_ext_sort ─────────────────────────────── */
int sort_ext_sort(void *base, size_t nmemb, size_t size, void *cmp, void *swap)
{
    (void)base;
    (void)nmemb;
    (void)size;
    (void)cmp;
    (void)swap;
    kprintf("[sort] sort_ext_sort: not yet implemented\n");
    return 0;
}
/* ── Stub: sort_ext_r_sort ─────────────────────────────── */
int sort_ext_r_sort(void *base, size_t nmemb, size_t size, void *cmp, void *swap, void *priv)
{
    (void)base;
    (void)nmemb;
    (void)size;
    (void)cmp;
    (void)swap;
    (void)priv;
    kprintf("[sort] sort_ext_r_sort: not yet implemented\n");
    return 0;
}
