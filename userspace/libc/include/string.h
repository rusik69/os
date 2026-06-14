#ifndef _STRING_H
#define _STRING_H

#include "unistd.h"

void *memcpy(void *dest, const void *src, unsigned long n);
void *memset(void *s, int c, unsigned long n);
int memcmp(const void *s1, const void *s2, unsigned long n);
unsigned long strlen(const char *s);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, unsigned long n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, unsigned long n);
char *strcat(char *dest, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

#endif /* _STRING_H */
