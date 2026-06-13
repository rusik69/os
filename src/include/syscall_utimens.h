#ifndef SYSCALL_UTIMENS_H
#define SYSCALL_UTIMENS_H

#include "types.h"

/* utimensat/futimens — nanosecond file timestamps */

#ifndef UTIME_NOW
#define UTIME_NOW    ((1L << 30) - 1)
#endif

#ifndef UTIME_OMIT
#define UTIME_OMIT   ((1L << 30) - 2)
#endif

/* AT_* flags */
#ifndef AT_FDCWD
#define AT_FDCWD             (-100)
#endif
#define AT_SYMLINK_NOFOLLOW  0x100
#define AT_REMOVEDIR         0x200

int sys_utimensat(int dirfd, const char *pathname,
                  const struct timespec times[2], int flags);
int sys_futimens(int fd, const struct timespec times[2]);

#endif /* SYSCALL_UTIMENS_H */
