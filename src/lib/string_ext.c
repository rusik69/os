#include "string_ext.h"
#include "string.h"     /* strlen, memcpy, tolower, isupper */
#include "heap.h"       /* kmalloc / kfree */

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
