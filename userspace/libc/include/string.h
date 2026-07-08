#ifndef _STRING_H
#define _STRING_H

#include "unistd.h"

void *memcpy(void *restrict dest, const void *restrict src, unsigned long n);
void *memset(void *s, int c, unsigned long n);
int memcmp(const void *s1, const void *s2, unsigned long n);
unsigned long strlen(const char *s);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, unsigned long n);
char *strcpy(char *restrict dest, const char *restrict src);
char *strncpy(char *restrict dest, const char *restrict src, unsigned long n);
char *strcat(char *restrict dest, const char *restrict src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

#endif /* _STRING_H */
