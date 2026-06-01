#ifndef SEARCH_H
#define SEARCH_H

#include "types.h"

/* ---- Sorting ---- */
void qsort_ext(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

/* Binary search over a sorted array. */
void *bsearch_ext(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* ---- Linear search (find key, return NULL if not found) ---- */
void *lfind(const void *key, const void *base, size_t *nmemb, size_t size,
            int (*compar)(const void *, const void *));

/* ---- Linear search with insert (append key if not found) ---- */
void *lsearch(const void *key, void *base, size_t *nmemb, size_t size,
              int (*compar)(const void *, const void *));

#endif /* SEARCH_H */
