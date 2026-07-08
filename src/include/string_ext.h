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

char *strcasestr(const char *haystack, const char *needle);
char *strdup_km(const char *s);          /* allocates via kmalloc */
char *strndup(const char *s, size_t n);
size_t strlcat(char *restrict dst, const char *restrict src, size_t size);
size_t strlcpy(char *restrict dst, const char *restrict src, size_t size);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
char *strchrnul(const char *s, int c);

#endif /* STRING_EXT_H */
