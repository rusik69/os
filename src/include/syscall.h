#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

/* Syscall numbers (Linux-compatible ABI) */
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_EXIT    4
#define SYS_GETPID  5
#define SYS_KILL    6
#define SYS_BRK     7
#define SYS_STAT    8
#define SYS_MKDIR   9
#define SYS_UNLINK  10
#define SYS_TIME    11
#define SYS_YIELD   12
#define SYS_UPTIME  13

void syscall_init(void);

/* Called from assembly stub - dispatches to the right handler */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5);

#endif
