#ifndef EVENTFD_H
#define EVENTFD_H

#include "types.h"

/* eventfd flags */
#define EFD_SEMAPHORE (1 << 0)
#define EFD_CLOEXEC   (1 << 1)
#define EFD_NONBLOCK  (1 << 2)

/* Create an eventfd — returns fd number or -1 on error */
int eventfd_create(uint32_t initval, int flags);

/* Read from an eventfd — reads the counter into *val.
 * Returns 0 on success, -1 on error (EAGAIN for non-blocking empty). */
int eventfd_read(int fd, uint64_t *val);

/* Write to an eventfd — adds val to the counter, wakes waiters.
 * Returns 0 on success, -1 on error (overflow, EAGAIN). */
int eventfd_write(int fd, uint64_t val);

/* Close an eventfd — frees the slot. */
void eventfd_close(int fd);

/* Syscall entry point (called from syscall dispatcher) */
int eventfd_syscall(uint32_t initval, int flags);

#endif /* EVENTFD_H */
