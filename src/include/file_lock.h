#ifndef FILE_LOCK_H
#define FILE_LOCK_H

#include "types.h"

/*
 * Lock type constants — compatible with POSIX fcntl F_SETLK/F_SETLKW.
 *
 * Mandatory locking: when `mandatory` is set on a lock, the kernel
 * enforces it on every read/write access — conflicting operations
 * return -EAGAIN (Item 53: mandatory lock violation detection).
 */
#define F_RDLCK  0   /* Shared / read lock */
#define F_WRLCK  1   /* Exclusive / write lock */
#define F_UNLCK  2   /* Release lock */

/* Forward declaration — struct file_lock is defined in vfs.h */
struct file_lock;

/* Initialize the file lock subsystem */
void file_lock_init(void);

/*
 * Set, release, or test a file lock using vfs.h's struct file_lock.
 *
 * For F_UNLCK: releases any lock held by the calling PID on the path.
 * For F_RDLCK/F_WRLCK: acquires an advisory or mandatory lock.
 *   - If flk->mandatory is set, the kernel enforces it — conflicting
 *     access from any process returns -EAGAIN.
 *   - If wait is 1 (F_SETLKW), the call blocks until the lock is
 *     available (not yet implemented — currently non-blocking only).
 *
 * Returns 0 on success, -EAGAIN on conflict, -EINVAL on bad args,
 * -ENOLCK if the lock table is full.
 */
int file_lock_set(const char *path, struct file_lock *flk, int wait);

/* Release a lock (convenience wrapper that sets type = F_UNLCK) */
int file_lock_unlock(const char *path, struct file_lock *flk);

/* Retrieve the current lock for a file path (returns -ENOENT if none) */
int file_lock_get(const char *path, struct file_lock *flk);

/*
 * Check whether a mandatory lock prevents the requested access.
 *
 * Called by the VFS layer before every read/write:
 *   - For a read operation (for_write=0), returns -EAGAIN if a mandatory
 *     write lock exists.
 *   - For a write operation (for_write=1), returns -EAGAIN if a mandatory
 *     read or write lock exists.
 *
 * Returns 0 if access is allowed, -EAGAIN if blocked by a mandatory lock,
 * or a negative error code on internal failure.
 */
int file_lock_check_mandatory(const char *path, int for_write);

#endif /* FILE_LOCK_H */
