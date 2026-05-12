#include "string.h"

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    while (n--) {
        if (*a != *b) return *a - *b;
        a++; b++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest + strlen(dest);
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest + strlen(dest);
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)0;
}

char *strrchr(const char *s, int c) {
    const char *found = (char *)0;
    while (*s) {
        if (*s == (char)c) found = s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)found;
}

char *strstr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char *)haystack;
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0) return (char *)haystack;
        haystack++;
    }
    return (char *)0;
}

long strtol(const char *nptr, char **endptr, int base) {
    while (isspace((unsigned char)*nptr)) nptr++;
    int sign = 1;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') nptr++;
    if (base == 0) {
        if (*nptr == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) { base = 16; nptr += 2; }
        else if (*nptr == '0') { base = 8; nptr++; }
        else base = 10;
    } else if (base == 16 && *nptr == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) {
        nptr += 2;
    }
    long val = 0;
    while (*nptr) {
        int digit;
        if (*nptr >= '0' && *nptr <= '9') digit = *nptr - '0';
        else if (*nptr >= 'a' && *nptr <= 'z') digit = *nptr - 'a' + 10;
        else if (*nptr >= 'A' && *nptr <= 'Z') digit = *nptr - 'A' + 10;
        else break;
        if (digit >= base) break;
        val = val * base + digit;
        nptr++;
    }
    if (endptr) *endptr = (char *)nptr;
    return sign * val;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    while (isspace((unsigned char)*nptr)) nptr++;
    if (*nptr == '+') nptr++;
    if (base == 0) {
        if (*nptr == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) { base = 16; nptr += 2; }
        else if (*nptr == '0') { base = 8; nptr++; }
        else base = 10;
    } else if (base == 16 && *nptr == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) {
        nptr += 2;
    }
    unsigned long val = 0;
    while (*nptr) {
        unsigned int digit;
        if (*nptr >= '0' && *nptr <= '9') digit = (unsigned int)(*nptr - '0');
        else if (*nptr >= 'a' && *nptr <= 'z') digit = (unsigned int)(*nptr - 'a' + 10);
        else if (*nptr >= 'A' && *nptr <= 'Z') digit = (unsigned int)(*nptr - 'A' + 10);
        else break;
        if (digit >= (unsigned int)base) break;
        val = val * (unsigned long)base + digit;
        nptr++;
    }
    if (endptr) *endptr = (char *)nptr;
    return val;
}

char *strtrim(char *s) {
    /* Skip leading whitespace */
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    /* Remove trailing whitespace */
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return (void *)0;
}

/* strtok: tokenize str on first call (non-NULL), subsequent calls pass NULL */
static char *strtok_saveptr = (char *)0;
char *strtok(char *str, const char *delim) {
    if (str) strtok_saveptr = str;
    if (!strtok_saveptr) return (char *)0;
    /* Skip leading delimiters */
    while (*strtok_saveptr && strchr(delim, *strtok_saveptr)) strtok_saveptr++;
    if (!*strtok_saveptr) return (char *)0;
    char *start = strtok_saveptr;
    /* Find end of token */
    while (*strtok_saveptr && !strchr(delim, *strtok_saveptr)) strtok_saveptr++;
    if (*strtok_saveptr) *strtok_saveptr++ = '\0';
    return start;
}

/* strsep: like strtok but caller manages state via pointer-to-pointer */
char *strsep(char **stringp, const char *delim) {
    char *s = *stringp;
    if (!s) return (char *)0;
    char *p = s;
    while (*p && !strchr(delim, *p)) p++;
    if (*p) { *p = '\0'; *stringp = p + 1; }
    else     *stringp = (char *)0;
    return s;
}

/* strspn: length of initial segment of s containing only chars in accept */
size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (*s && strchr(accept, *s)) { n++; s++; }
    return n;
}

/* strcspn: length of initial segment of s NOT containing any char in reject */
size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (*s && !strchr(reject, *s)) { n++; s++; }
    return n;
}

/* strpbrk: find first occurrence in s of any char from accept; NULL if none */
char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        if (strchr(accept, *s)) return (char *)s;
        s++;
    }
    return (char *)0;
}
