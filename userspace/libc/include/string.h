#ifndef _STRING_H
#define _STRING_H

#include "unistd.h"

void *memcpy(void *restrict dest, const void *restrict src, unsigned long n) __attribute__((__nonnull__));
void *memset(void *s, int c, unsigned long n) __attribute__((__nonnull__));
int memcmp(const void *s1, const void *s2, unsigned long n) __attribute__((__nonnull__));
unsigned long strlen(const char *s) __attribute__((__nonnull__));
int strcmp(const char *s1, const char *s2) __attribute__((__nonnull__));
int strncmp(const char *s1, const char *s2, unsigned long n) __attribute__((__nonnull__));
char *strcpy(char *restrict dest, const char *restrict src) __attribute__((__nonnull__));
char *strncpy(char *restrict dest, const char *restrict src, unsigned long n) __attribute__((__nonnull__));
char *strcat(char *restrict dest, const char *restrict src) __attribute__((__nonnull__));
char *strchr(const char *s, int c) __attribute__((__nonnull__));
char *strrchr(const char *s, int c) __attribute__((__nonnull__));
char *strstr(const char *haystack, const char *needle) __attribute__((__nonnull__));

#endif /* _STRING_H */
