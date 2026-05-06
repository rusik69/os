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

/* Extended syscall set used by command-side libc wrappers. */
#define SYS_FS_FORMAT     100
#define SYS_FS_CREATE     101
#define SYS_FS_WRITE      102
#define SYS_FS_READ       103
#define SYS_FS_DELETE     104
#define SYS_FS_LIST       105
#define SYS_FS_STAT       106
#define SYS_FS_STAT_EX    107
#define SYS_FS_CHMOD      108
#define SYS_FS_CHOWN      109
#define SYS_FS_GET_USAGE  110
#define SYS_FS_LIST_NAMES 111
#define SYS_ATA_PRESENT   112
#define SYS_VFS_READ      113
#define SYS_VFS_WRITE     114
#define SYS_VFS_STAT      115
#define SYS_VFS_CREATE    116
#define SYS_VFS_UNLINK    117
#define SYS_VFS_READDIR   118
#define SYS_WAITPID       119
#define SYS_SLEEP_TICKS   120
#define SYS_ATA_SECTORS   121
#define SYS_AHCI_PRESENT  122
#define SYS_AHCI_SECTORS  123

void syscall_init(void);

/* Called from assembly stub - dispatches to the right handler */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5);

#endif
