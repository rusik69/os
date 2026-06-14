#ifndef _STDIO_H
#define _STDIO_H

#include "unistd.h"

#define EOF (-1)

/* Minimal printf */
int printf(const char *fmt, ...);
int putchar(int c);
int puts(const char *s);

#endif /* _STDIO_H */
