/*
 * SPDX-License-Identifier: MIT
 *
 * stdio.h — Standard I/O function declarations for the OS libc.
 *
 * Architecture
 * ============
 * This header provides the user-space C standard I/O interface. The
 * implementation is intentionally minimal — a freestanding kernel
 * environment does not need the full stdio buffered-I/O FILE* model.
 * Instead, output is either:
 *
 *   (a) direct character output via the debug serial port / VGA console
 *       (putchar), or
 *   (b) formatted via printf-family routines that call putchar internally.
 *
 * All formatting is handled by the shared printf engine in src/lib/printf.c,
 * which supports %d, %u, %x, %s, %c, %p and %% specifiers.
 *
 * Limitations (intentional)
 * -------------------------
 * - No FILE* / fopen / fclose / fread / fwrite — those belong in a
 *   hosted libc; this kernel targets freestanding user-space.
 * - No buffering — every putchar() call is an immediate write.  Future
 *   work could add a write-buffer layer to coalesce small writes.
 * - No scanf / fgets — input parsing is currently application-specific.
 * - snprintf() is available for bounded formatting to a string buffer.
 *
 * Return convention
 * -----------------
 * All functions return the number of characters written, or a negative
 * value (typically -1) on error.  putchar() returns the character cast
 * to unsigned char on success, EOF on error.  snprintf() returns the
 * number of bytes that would have been written (excluding the NUL) if
 * the buffer were large enough, following C99 semantics.
 */

#ifndef _STDIO_H
#define _STDIO_H

#include "unistd.h"

#define EOF (-1)

/**
 * printf — Formatted print to the console.
 * @fmt:  printf-style format string.
 * @...:  Variable arguments matching the format specifiers.
 *
 * Writes the formatted output character-by-character via putchar().
 * Supported: %% %d %u %x %X %s %c %p.
 *
 * Return: The number of characters written (not including the trailing
 *         NUL that snprintf would suppress), or a negative error code.
 */
int printf(const char *fmt, ...);

/**
 * snprintf — Bounded formatted print to a string buffer.
 * @buf:  Destination buffer (may be NULL when @size is 0).
 * @size: Size of @buf in bytes.
 * @fmt:  printf-style format string.
 * @...:  Variable arguments.
 *
 * Writes at most @size-1 characters into @buf and always NUL-terminates.
 * If @size == 0, nothing is written but the total needed length is still
 * returned (C99 snprintf behaviour).
 *
 * Return: The number of bytes that *would* have been written (excluding
 *         the NUL) had @size been sufficiently large — i.e. the length
 *         of the format expansion.
 */
int snprintf(char *buf, unsigned long size, const char *fmt, ...);

/**
 * putchar — Write a single character to the console.
 * @c:  Character to write (cast to unsigned char internally).
 *
 * Writes @c to the active console output (serial port or VGA text
 * framebuffer, depending on platform configuration).
 *
 * Return: @c cast to unsigned char on success, EOF (-1) on error.
 */
int putchar(int c);

/**
 * puts — Write a NUL-terminated string followed by a newline.
 * @s:  NUL-terminated string.
 *
 * Equivalent to: printf("%s\n", s).  The trailing newline is always
 * appended.
 *
 * Return: Non-negative on success, EOF (-1) on error.
 */
int puts(const char *s);

#endif /* _STDIO_H */
