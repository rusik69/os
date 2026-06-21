#include "string_ext.h"
#include "string.h"     /* strlen, memcpy, tolower, isupper */
#include "heap.h"       /* kmalloc / kfree */
#include "signal.h"     /* signal definitions for strsignal */

/* ── Signal name table for strsignal ─────────────────────────────── */

static const char * const sig_names[] = {
    NULL,           /* 0 - not a signal */
    "SIGHUP",       /* 1 */
    "SIGINT",       /* 2 */
    "SIGQUIT",      /* 3 */
    "SIGILL",       /* 4 */
    "SIGTRAP",      /* 5 */
    "SIGABRT",      /* 6 */
    "SIGBUS",       /* 7 */
    "SIGFPE",       /* 8 */
    "SIGKILL",      /* 9 */
    "SIGUSR1",      /* 10 */
    "SIGSEGV",      /* 11 */
    "SIGUSR2",      /* 12 */
    "SIGPIPE",      /* 13 */
    "SIGALRM",      /* 14 */
    "SIGTERM",      /* 15 */
    "SIGSTKFLT",    /* 16 */
    "SIGCHLD",      /* 17 */
    "SIGCONT",      /* 18 */
    "SIGSTOP",      /* 19 */
    "SIGTSTP",      /* 20 */
    "SIGTTIN",      /* 21 */
    "SIGTTOU",      /* 22 */
    "SIGURG",       /* 23 */
    "SIGXCPU",      /* 24 */
    "SIGXFSZ",      /* 25 */
    "SIGVTALRM",    /* 26 */
    "SIGPROF",      /* 27 */
    "SIGWINCH",     /* 28 */
    "SIGIO",        /* 29 */
    NULL,           /* 30 */
    "SIGSYS",       /* 31 */
};

/*
 * strcasestr — case-insensitive strstr.
 */
char *strcasestr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char *)haystack;
    while (*haystack) {
        int match = 1;
        for (size_t i = 0; i < nlen; i++) {
            if (tolower((unsigned char)haystack[i]) != tolower((unsigned char)needle[i])) {
                match = 0;
                break;
            }
        }
        if (match) return (char *)haystack;
        haystack++;
    }
    return (char *)0;
}

/*
 * strdup — duplicate string via kmalloc.
 * Caller must kfree() the result.
 */
char *strdup_km(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = (char *)kmalloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

/*
 * strndup — duplicate first n characters (or up to NUL) via kmalloc.
 * Result is always NUL-terminated.  Caller must kfree().
 */
char *strndup(const char *s, size_t n) {
    size_t slen = strlen(s);
    if (slen > n) slen = n;
    char *p = (char *)kmalloc(slen + 1);
    if (p) {
        memcpy(p, s, slen);
        p[slen] = '\0';
    }
    return p;
}

/*
 * strlcat — safe concatenation with size bound.
 * Returns the total length the string would have (like BSD strlcat).
 * If the return value >= size, truncation occurred.
 */
#ifndef TEST_MODE_HOST
size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dlen = 0;
    while (dlen < size && dst[dlen]) dlen++;
    if (dlen == size) return dlen + strlen(src);
    size_t slen = strlen(src);
    size_t remaining = size - dlen - 1;
    size_t to_copy = slen < remaining ? slen : remaining;
    memcpy(dst + dlen, src, to_copy);
    dst[dlen + to_copy] = '\0';
    return dlen + slen;
}

/*
 * strlcpy — safe copy with size bound.
 * Always NUL-terminates (if size > 0).
 * Returns the length of src (like BSD strlcpy).
 * If the return value >= size, truncation occurred.
 */
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t slen = strlen(src);
    if (size == 0) return slen;
    size_t to_copy = slen < size - 1 ? slen : size - 1;
    memcpy(dst, src, to_copy);
    dst[to_copy] = '\0';
    return slen;
}
#endif /* not TEST_MODE_HOST */

/*
 * strcasecmp — case-insensitive compare.
 */
int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

/*
 * strncasecmp — case-insensitive compare with length limit.
 */
int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

/*
 * strchrnul — find first occurrence of c in s, or return pointer to NUL terminator.
 */
char *strchrnul(const char *s, int c) {
    while (*s && *s != (char)c) s++;
    return (char *)s;
}

/* ── strsignal — map signal number to human-readable name ───────── */

const char *strsignal(int signum) {
    if (signum >= 1 && signum < (int)(sizeof(sig_names) / sizeof(sig_names[0]))) {
        const char *name = sig_names[signum];
        if (name) return name;
    }
    return "Unknown signal";
}

/* ── memmem — find byte string in memory (GNU/POSIX extension) ──── */

void *memmem(const void *haystack, size_t haystacklen,
             const void *needle, size_t needlelen) {
    if (!haystack || !needle) return NULL;
    if (needlelen == 0) return (void *)haystack;
    if (needlelen > haystacklen) return NULL;

    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    size_t last = haystacklen - needlelen;

    for (size_t i = 0; i <= last; i++) {
        size_t j;
        for (j = 0; j < needlelen; j++) {
            if (h[i + j] != n[j]) break;
        }
        if (j == needlelen) return (void *)(h + i);
    }
    return NULL;
}

/* ── strnlen_user ─────────────────────────────── */
size_t strnlen_user(const char *s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len])
        len++;
    return len;
}
/* ── strncpy_from_user ─────────────────────────────── */
int strncpy_from_user(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = src[i];
    if (i < n)
        dst[i] = '\0';
    return (int)i;
}
/* ── strcmp_user ─────────────────────────────── */
int strcmp_user(const char *cs, const char *ct)
{
    while (*cs && *ct && *cs == *ct) {
        cs++;
        ct++;
    }
    return (int)(unsigned char)*cs - (int)(unsigned char)*ct;
}
