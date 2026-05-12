#include "stdlib.h"
#include "string.h"

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
 * strdup — heap-allocate a copy of s.  Caller must free() the result.
 */
char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = (char *)malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

/* ---- Integer to ASCII conversions ---- */

/*
 * itoa — convert integer to string in given base (2–36).
 * buf must be large enough (at least 34 bytes for base-2).
 * Returns buf.
 */
char *itoa(int value, char *buf, int base) {
    if (base < 2 || base > 36) { buf[0] = '\0'; return buf; }
    char tmp[34];
    int i = 0;
    int neg = 0;
    unsigned int uval;
    if (value < 0 && base == 10) { neg = 1; uval = (unsigned int)(-(value + 1)) + 1u; }
    else uval = (unsigned int)value;
    if (uval == 0) { tmp[i++] = '0'; }
    while (uval) {
        unsigned int rem = uval % (unsigned int)base;
        tmp[i++] = (char)(rem < 10 ? '0' + rem : 'a' + rem - 10);
        uval /= (unsigned int)base;
    }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

/*
 * ltoa — like itoa but for long.
 */
char *ltoa(long value, char *buf, int base) {
    if (base < 2 || base > 36) { buf[0] = '\0'; return buf; }
    char tmp[66];
    int i = 0;
    int neg = 0;
    unsigned long uval;
    if (value < 0 && base == 10) { neg = 1; uval = (unsigned long)(-(value + 1)) + 1ul; }
    else uval = (unsigned long)value;
    if (uval == 0) { tmp[i++] = '0'; }
    while (uval) {
        unsigned long rem = uval % (unsigned long)base;
        tmp[i++] = (char)(rem < 10 ? '0' + (unsigned long)rem : 'a' + (unsigned long)rem - 10);
        uval /= (unsigned long)base;
    }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

/* ---- Pseudo-random number generator (LCG, period 2^32) ---- */

static unsigned long rand_state = 12345;

void srand(unsigned int seed) {
    rand_state = (unsigned long)seed;
}

int rand(void) {
    rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((rand_state >> 33) & 0x7FFFFFFF);
}

/* ---- getopt — POSIX-style option parser ---- */

char  *optarg = (char *)0;
int    optind = 1;
int    opterr = 1;
int    optopt = 0;

/* Reset for re-use within the same command */
static int optpos = 1; /* position within current argv element */
static void getopt_reset(void) { optind = 1; optarg = (char *)0; optopt = 0; optpos = 1; }

/*
 * getopt — parse short options from argv.
 * Returns option character, '?' for unknown, ':' for missing arg, -1 when done.
 * Supports options with required arguments (colon after letter in optstring).
 *
 * argc/argv: use a local array of two pointers { progname, arg } for single-arg
 * commands, or build argv from the parsed args string.
 */
int getopt(int argc, char * const argv[], const char *optstring) {
    if (optind >= argc) { getopt_reset(); return -1; }
    if (argv[optind][0] != '-' || argv[optind][1] == '\0') {
        getopt_reset(); return -1;  /* non-option arg stops processing */
    }
    if (argv[optind][0] == '-' && argv[optind][1] == '-') {
        optind++; getopt_reset(); return -1; /* -- ends options */
    }

    int opt = (unsigned char)argv[optind][optpos];
    optpos++;

    /* Move to next argv if we consumed this one */
    if (argv[optind][optpos] == '\0') { optind++; optpos = 1; }

    /* Look for opt in optstring */
    const char *os = optstring;
    while (*os) {
        if (*os == opt) {
            if (os[1] == ':') {
                /* requires argument */
                if (optind < argc) {
                    optarg = argv[optind++];
                } else {
                    optarg = (char *)0;
                    optopt = opt;
                    return ':';
                }
            }
            return opt;
        }
        os++;
    }
    optopt = opt;
    return '?';
}

/* ---- fnmatch — shell-style glob pattern matching ---- */
/* Flags (currently unused but accepted for compat) */
#define FNM_NOMATCH 1

/*
 * fnmatch — match string against shell glob pattern.
 * Supports: * (any sequence), ? (one char), [abc] / [a-z] character classes.
 * Returns 0 on match, FNM_NOMATCH otherwise.
 */
int fnmatch(const char *pattern, const char *string, int flags) {
    (void)flags;
    const char *p = pattern, *s = string;

    while (*p) {
        if (*p == '*') {
            /* Collapse consecutive stars */
            while (*p == '*') p++;
            if (!*p) return 0; /* trailing * matches everything */
            /* Try matching from every position in s */
            while (*s) {
                if (fnmatch(p, s, 0) == 0) return 0;
                s++;
            }
            return FNM_NOMATCH;
        } else if (*p == '?') {
            if (!*s) return FNM_NOMATCH;
            p++; s++;
        } else if (*p == '[') {
            if (!*s) return FNM_NOMATCH;
            p++; /* skip '[' */
            int negate = 0;
            if (*p == '!' || *p == '^') { negate = 1; p++; }
            int matched = 0;
            while (*p && *p != ']') {
                if (*(p+1) == '-' && *(p+2) && *(p+2) != ']') {
                    if ((unsigned char)*s >= (unsigned char)*p &&
                        (unsigned char)*s <= (unsigned char)*(p+2)) matched = 1;
                    p += 3;
                } else {
                    if ((unsigned char)*s == (unsigned char)*p) matched = 1;
                    p++;
                }
            }
            if (*p == ']') p++;
            if (matched == negate) return FNM_NOMATCH;
            s++;
        } else {
            if (*p != *s) return FNM_NOMATCH;
            p++; s++;
        }
    }
    return (*s == '\0') ? 0 : FNM_NOMATCH;
}
