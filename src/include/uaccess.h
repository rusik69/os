#ifndef UACCESS_H
#define UACCESS_H

#include "types.h"

/*
 * User access functions — safe copy to/from user-space buffers.
 *
 * SMAP (Supervisor Mode Access Prevention) prevents the kernel from
 * directly accessing user pages.  These wrappers temporarily toggle
 * the AC flag (via STAC/CLAC) so the underlying copy works.
 *
 * All functions validate the user range before copying.
 * On failure they return -EFAULT and leave the destination undefined.
 */

/* Copy from user-space to kernel buffer.  Returns 0 on success, -EFAULT on error. */
int copy_from_user(void *dst, uint64_t src_user, size_t n);

/* Copy to user-space from kernel buffer.  Returns 0 on success, -EFAULT on error. */
int copy_to_user(uint64_t dst_user, const void *src, size_t n);

/* Copy a null-terminated string from user-space to kernel buffer.
 * Returns number of bytes copied (including NUL terminator) on success,
 * or -EFAULT on error.  Always NUL-terminates dst (up to max_len-1). */
long strncpy_from_user(char *dst, uint64_t src_user, size_t max_len);

/* Return the length of a user-space string (including NUL), up to max_len.
 * Returns length (>= 1) on success, -EFAULT on error. */
long strlen_user(uint64_t src_user, size_t max_len);

/* Read a single byte from user-space.  Returns 0 on success, -EFAULT on error. */
int get_user_byte(uint64_t addr, uint8_t *out);

/* Write a single byte to user-space.  Returns 0 on success, -EFAULT on error. */
int put_user_byte(uint64_t addr, uint8_t val);

/* Zero a user-space buffer.  Returns 0 on success, -EFAULT on error. */
int memset_user(uint64_t dst_user, uint8_t val, size_t n);

#endif /* UACCESS_H */
