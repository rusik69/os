#ifndef STRING_EXT_H
#define STRING_EXT_H

#include "types.h"     /* size_t, NULL */

/*
 * Extended string operations.
 *
 * Functions already available via string.h + string.c:
 *   strstr, strtok, strtok_r, strsep, memmove, strrchr, strlen, memcpy, …
 *
 * Additional functions declared here (implemented in string_ext.c):
 */

char *strcasestr(const char *haystack, const char *needle) __nonnull;
char *strdup_km(const char *s) __nonnull;
char *strndup(const char *s, size_t n) __nonnull;
size_t strlcat(char *restrict dst, const char *restrict src, size_t size) __nonnull;
size_t strlcpy(char *restrict dst, const char *restrict src, size_t size) __nonnull;
int strcasecmp(const char *s1, const char *s2) __nonnull;
int strncasecmp(const char *s1, const char *s2, size_t n) __nonnull;
char *strchrnul(const char *s, int c) __nonnull;

#endif /* STRING_EXT_H */
