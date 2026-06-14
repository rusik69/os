/* Syscall wrappers that need C logic (fstat via fstatat, readlink via readlinkat, etc.) */

#include "unistd.h"

/* fstat(fd, buf) — use fstatat with AT_EMPTY_PATH */
int fstat(int fd, struct stat *buf) {
    return fstatat(fd, "", buf, AT_EMPTY_PATH);
}

/* readlink(path, buf, size) — use readlinkat with AT_FDCWD */
int readlink(const char *path, char *buf, unsigned long size) {
    return readlinkat(AT_FDCWD, path, buf, size);
}

/* setpgrp() — set process group to own PID */
int setpgrp(void) {
    return setpgid(0, 0);
}

/* getpgrp() — get process group ID */
int getpgrp(void) {
    return getpgid(0);
}
