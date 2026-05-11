#ifndef STDLIB_H
#define STDLIB_H

#include "types.h"
#include "string.h"   /* for strtol/strtoul used by atoi/atol */

/* ---- Simple number conversion (inline wrappers) ---- */
static inline int  atoi(const char *s) { return (int)strtol(s, (char **)0, 10); }
static inline long atol(const char *s) { return strtol(s, (char **)0, 10); }

/* ---- Absolute value (inline) ---- */
static inline int  abs(int  n) { return n < 0 ? -n : n; }
static inline long labs(long n) { return n < 0 ? -n : n; }

/* ---- Min / max macros ---- */
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

/* ---- Clamp macro ---- */
#define clamp(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* ---- Sorting and searching ---- */
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* ---- Heap-allocated string copy ---- */
char *strdup(const char *s);

#endif
