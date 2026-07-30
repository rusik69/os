#ifndef _STDLIB_H
#define _STDLIB_H

/**
 * @file stdlib.h
 * @brief Standard library — memory allocation, string conversion, and process control.
 *
 * Implementation status summary:
 * ==============================
 *
 * MEMORY ALLOCATION  (userspace/libc/stdlib.c)
 * --------------------------------------------
 *   void *malloc(unsigned long size);    ✓ Implemented — bump allocator + free list
 *   void free(void *ptr);                ✓ Implemented — coalesces adjacent free blocks
 *   void *calloc(unsigned long nmemb, unsigned long size);  ✓ Implemented — malloc + memset(0)
 *   void *realloc(void *ptr, unsigned long size);            ✓ Implemented — reuses block if big enough
 *
 *   Design: The heap uses a simple bump-allocator backed by brk(). Freed blocks
 *   are kept on a sorted free list and reused. Adjacent free blocks are coalesced
 *   on free. Minimum allocation granularity is 16 bytes, with a minimum block
 *   size of 32 bytes. The free header (free_hdr_t) lives immediately before the
 *   user pointer and stores the total block size. Double-free detection is
 *   performed by checking if the header's size field is zero.
 *
 * STRING CONVERSION  (userspace/libc/stdlib.c)
 * --------------------------------------------
 *   int atoi(const char *s);             ✓ Implemented — trims whitespace, handles +/- sign
 *   long strtol(const char *s, ...);     ✗ NOT IMPLEMENTED — atoi only
 *   unsigned long strtoul(...);          ✗ NOT IMPLEMENTED
 *
 * PROCESS CONTROL  (userspace/libc/stdlib.c)
 * -------------------------------------------
 *   void abort(void);                    ✓ Implemented — writes "Abort\n" to stderr, loops exit(1)
 *   void srand(unsigned int seed);       ✓ Implemented — seeds LCG PRNG
 *   int rand(void);                      ✓ Implemented — LCG: (1103515245 * seed + 12345) / 65536 % 32768
 *
 *   void exit(int status);              ✗ NOT IMPLEMENTED — use syscall SYS_EXIT via unistd.h
 *   int atexit(void (*func)(void));      ✗ NOT IMPLEMENTED
 *
 * MISSING (not yet available)
 * ---------------------------
 *   Standard POSIX functions not yet implemented:
 *   - strtol / strtoul / strtoull / strtoll (string → long integer)
 *   - strtod / strtof (string → floating point)
 *   - abs / labs (integer absolute value)
 *   - div / ldiv / lldiv (integer division with remainder)
 *   - qsort / bsearch (sorting and searching)
 *   - exit / atexit / _Exit (program termination with cleanup)
 *   - getenv / setenv / putenv / unsetenv (environment variables)
 *   - system (shell command execution)
 *   - mblen / mbtowc / wctomb (multi-byte character support)
 *   - btowc / wctob (wide character conversions)
 *
 * Linking:
 *   Applications that need the missing functions can implement them locally
 *   using the existing primitives (malloc, free, atoi) or add them to
 *   userspace/libc/stdlib.c.
 */

#include "unistd.h"

/* Memory allocation */
void *malloc(unsigned long size);
void free(void *ptr);
void *calloc(unsigned long nmemb, unsigned long size);
void *realloc(void *ptr, unsigned long size);

/* String conversion */
int atoi(const char *s);

/* Process control */
void abort(void);
void srand(unsigned int seed);
int rand(void);

#endif /* _STDLIB_H */
