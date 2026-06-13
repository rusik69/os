#ifndef SYSCALL_NEW_H
#define SYSCALL_NEW_H

#include "types.h"

/* ── dup/dup2/dup3 ──────────────────────────────────────────────────── */
int do_dup(int old_fd);
int do_dup2(int old_fd, int new_fd);
int do_dup3(int old_fd, int new_fd, int flags);

/* ── pipe2 ──────────────────────────────────────────────────────────── */
int do_pipe2(int fds[2], int flags);

/* ── sysinfo ────────────────────────────────────────────────────────── */
struct sysinfo;
int do_sysinfo(struct sysinfo *info);

/* ── getrandom ──────────────────────────────────────────────────────── */
int do_getrandom(void *buf, size_t count, unsigned int flags);

/* ── prctl ──────────────────────────────────────────────────────────── */
int do_prctl(int option, unsigned long arg2, unsigned long arg3,
             unsigned long arg4, unsigned long arg5);

/* ── utimensat / futimens ───────────────────────────────────────────── */
struct timespec;
int do_utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags);
int do_futimens(int fd, const struct timespec times[2]);

/* ── posix_fadvise ──────────────────────────────────────────────────── */
int do_fadvise64(int fd, uint64_t offset, uint64_t len, int advice);

/* ── readlinkat / symlinkat ─────────────────────────────────────────── */
int do_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz);
int do_symlinkat(const char *target, int newdirfd, const char *linkpath);

#endif /* SYSCALL_NEW_H */
