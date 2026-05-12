#ifndef STDLIB_H
#define STDLIB_H

#include "types.h"
#include "string.h"   /* for strtol/strtoul used by atoi/atol */
#include "libc.h"     /* for libc_malloc/free backing malloc/free */

/* ---- Heap allocation (backed by SYS_MALLOC / SYS_FREE) ---- */
static inline void *malloc(size_t size)            { return libc_malloc(size); }
static inline void  free(void *ptr)                { libc_free(ptr); }
static inline void *calloc(size_t n, size_t sz)    { return libc_calloc(n, sz); }
static inline void *realloc(void *ptr, size_t sz)  { return libc_realloc(ptr, sz); }

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

/* ---- Integer to ASCII conversions ---- */
char *itoa(int value, char *buf, int base);
char *ltoa(long value, char *buf, int base);

/* ---- Pseudo-random number generator ---- */
void srand(unsigned int seed);
int  rand(void);
#define RAND_MAX 0x7FFFFFFF

/* ---- POSIX option parser ---- */
extern char *optarg;
extern int   optind;
extern int   opterr;
extern int   optopt;
int getopt(int argc, char * const argv[], const char *optstring);

/* ---- Shell-style glob matching ---- */
#define FNM_NOMATCH 1
int fnmatch(const char *pattern, const char *string, int flags);

#endif
