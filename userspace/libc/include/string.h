/*
 * SPDX-License-Identifier: MIT
 *
 * string.h — String and memory function declarations for the OS libc.
 *
 * Architecture
 * ============
 * This header provides the user-space C string and memory manipulation
 * interface.  All implementations live in userspace/libc/string.c and
 * operate on raw memory / NUL-terminated strings with no external
 * dependencies beyond the C language itself.
 *
 * Function categories
 * -------------------
 *   1. Memory block functions (memcpy, memset, memcmp)
 *      - Operate on `void *` buffers of exactly `n` bytes.
 *      - memcpy() and memset() return the destination pointer.
 *      - memcmp() returns zero if equal, <0 or >0 on difference.
 *
 *   2. String length / comparison (strlen, strcmp, strncmp)
 *      - strlen() counts characters until the terminating NUL.
 *      - strcmp() compares until a difference or NUL is found.
 *      - strncmp() stops at NUL or after exactly `n` characters.
 *      - All accept NUL-terminated strings; behaviour on non-terminated
 *        inputs is undefined (potential overread).
 *
 *   3. String copy / concatenation (strcpy, strncpy, strcat)
 *      - strcpy() copies the entire source including NUL terminator.
 *      - strncpy() pads the destination with NUL up to `n` bytes.
 *      - strcat() appends to the end of an existing string.
 *      - All expect non-overlapping, sufficiently large destinations.
 *
 *   4. String search (strchr, strrchr, strstr)
 *      - strchr()  — find first occurrence of a character.
 *      - strrchr() — find last  occurrence of a character.
 *      - strstr()  — find first occurrence of a substring.
 *      - All return a pointer into the searched string, or NULL.
 *
 * Return convention
 * -----------------
 * - memcpy / memset / strcpy / strncpy / strcat return the destination
 *   pointer (which is always equal to the input dest pointer).
 * - strlen returns the string length (does not include the NUL).
 * - memcmp / strcmp / strncmp return 0 on equality, negative if the
 *   first differing character in s1 is smaller than in s2, positive
 *   otherwise.
 * - strchr / strrchr / strstr return a pointer to the found location,
 *   or NULL if nothing was found.
 *
 * Parameter contract
 * ------------------
 * All pointer parameters are required to be non-null and valid.  The
 * declarations carry __attribute__((__nonnull__)) so the compiler may
 * emit warnings or make optimisation assumptions based on non-null
 * semantics.  Passing NULL results in undefined behaviour (likely a
 * page fault).
 *
 * Limitations (not yet implemented)
 * ----------------------------------
 *   - strncat, strlcat              — bounded concatenation
 *   - memmove                       — overlap-safe memory copy
 *   - strdup, strndup               — heap-allocated string duplication
 *   - strtok, strtok_r              — tokenisation
 *   - strcoll, strxfrm              — locale-aware comparison
 *   - memchr                        — byte search in fixed-length buffer
 *   - strerror                      — errno-to-string mapping
 *   - strspn, strcspn, strpbrk      — span / break functions
 *
 * These functions may be added as the libc grows toward broader POSIX
 * coverage.
 */

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
