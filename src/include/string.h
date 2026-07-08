#ifndef STRING_H
#define STRING_H

#include "types.h"

void *memset(void *s, int c, size_t n) __nonnull;
void *memcpy(void *restrict dest, const void *restrict src, size_t n) __nonnull;
void *memmove(void *dest, const void *src, size_t n) __nonnull;
int memcmp(const void *s1, const void *s2, size_t n) __nonnull;
size_t strlen(const char *s) __nonnull;
int strcmp(const char *s1, const char *s2) __nonnull;
int strncmp(const char *s1, const char *s2, size_t n) __nonnull;
char *strcpy(char *restrict dest, const char *restrict src) __nonnull;
char *strncpy(char *restrict dest, const char *restrict src, size_t n) __nonnull;
char *strcat(char *restrict dest, const char *restrict src) __nonnull;
char *strncat(char *restrict dest, const char *restrict src, size_t n) __nonnull;
char *strchr(const char *s, int c) __nonnull;
char *strrchr(const char *s, int c) __nonnull;
char *strstr(const char *haystack, const char *needle) __nonnull;
long strtol(const char *nptr, char **endptr, int base) __attribute__((__nonnull__(1)));
unsigned long strtoul(const char *nptr, char **endptr, int base) __attribute__((__nonnull__(1)));
char *strtrim(char *s) __nonnull;
void *memchr(const void *s, int c, size_t n) __nonnull;
char *strtok(char *str, const char *delim) __attribute__((__nonnull__(2)));
char *strtok_r(char *str, const char *delim, char **saveptr) __attribute__((__nonnull__(2, 3)));
char *strsep(char **stringp, const char *delim) __attribute__((__nonnull__(1, 2)));
size_t strspn(const char *s, const char *accept) __nonnull;
size_t strcspn(const char *s, const char *reject) __nonnull;
char *strpbrk(const char *s, const char *accept) __nonnull;
size_t strnlen(const char *s, size_t maxlen) __nonnull;
void *memccpy(void *restrict dest, const void *restrict src, int c, size_t n) __nonnull;

/* Search for a byte string in a memory region (like strstr but for binary data) */
void *memmem(const void *haystack, size_t haystacklen,
             const void *needle, size_t needlelen);

/* Return error string for errno value (POSIX strerror) */
char *strerror(int errnum);

/* Return human-readable signal name string */
const char *strsignal(int signum);

/* Character classification — inline for zero overhead */
static inline int isdigit(int c)  { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline int isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int isalnum(int c)  { return isalpha(c) || isdigit(c); }
static inline int isspace(int c)  {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
static inline int isprint(int c)  { return c >= 32 && c < 127; }
static inline int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c)  { return c >= 'a' && c <= 'z'; }
static inline int toupper(int c)  { return islower(c) ? c - 'a' + 'A' : c; }
static inline int tolower(int c)  { return isupper(c) ? c - 'A' + 'a' : c; }

#endif
