#include "stdlib_ext.h"
#include "string.h"     /* isspace, strtol/strtoul base */

/*
 * atoll — convert string to long long (base 10).
 */
long long atoll(const char *nptr) {
    return strtoll(nptr, (char **)0, 10);
}

/*
 * strtoll — convert string to long long with base detection.
 */
long long strtoll(const char *nptr, char **endptr, int base) {
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
    long long val = 0;
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

/*
 * strtoull — convert string to unsigned long long.
 */
unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    while (isspace((unsigned char)*nptr)) nptr++;
    if (*nptr == '+') nptr++;
    if (base == 0) {
        if (*nptr == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) { base = 16; nptr += 2; }
        else if (*nptr == '0') { base = 8; nptr++; }
        else base = 10;
    } else if (base == 16 && *nptr == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) {
        nptr += 2;
    }
    unsigned long long val = 0;
    while (*nptr) {
        unsigned int digit;
        if (*nptr >= '0' && *nptr <= '9') digit = (unsigned int)(*nptr - '0');
        else if (*nptr >= 'a' && *nptr <= 'z') digit = (unsigned int)(*nptr - 'a' + 10);
        else if (*nptr >= 'A' && *nptr <= 'Z') digit = (unsigned int)(*nptr - 'A' + 10);
        else break;
        if (digit >= (unsigned int)base) break;
        val = val * (unsigned long long)base + digit;
        nptr++;
    }
    if (endptr) *endptr = (char *)nptr;
    return val;
}

/*
 * ultoa — convert unsigned long to string in given base (2–36).
 * buf must be large enough (at least 66 bytes for base-2).
 * Returns buf.
 */
char *ultoa(unsigned long value, char *str, int base) {
    if (base < 2 || base > 36) { str[0] = '\0'; return str; }
    char tmp[66];
    int i = 0;
    if (value == 0) { tmp[i++] = '0'; }
    while (value) {
        unsigned long rem = value % (unsigned long)base;
        tmp[i++] = (char)(rem < 10 ? '0' + rem : 'a' + rem - 10);
        value /= (unsigned long)base;
    }
    int j = 0;
    while (i > 0) str[j++] = tmp[--i];
    str[j] = '\0';
    return str;
}

/* ── Stub: realloc ─────────────────────────────── */
void* realloc(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;
    kprintf("[stdlib] realloc: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: calloc ─────────────────────────────── */
void* calloc(size_t nmemb, size_t size)
{
    (void)nmemb;
    (void)size;
    kprintf("[stdlib] calloc: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: strtol ─────────────────────────────── */
long strtol(const char *nptr, char **endptr, int base)
{
    (void)nptr;
    (void)endptr;
    (void)base;
    kprintf("[stdlib] strtol: not yet implemented\n");
    return -ENOSYS;
}
