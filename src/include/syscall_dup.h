#ifndef SYSCALL_DUP_H
#define SYSCALL_DUP_H

/* dup/dup2/dup3 syscalls */
int do_dup(int old_fd);
int do_dup2(int old_fd, int new_fd);
int do_dup3(int old_fd, int new_fd, int flags);

#endif
