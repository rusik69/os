/*
 * syscall_linux.c — Linux x86-64 ABI syscall dispatch table
 *
 * Implements sys_call_table[], an array of function pointers indexed by
 * Linux __NR_* syscall numbers (0–334). Each entry either points to a
 * Linux-compatible wrapper that translates arguments and forwards to the
 * corresponding internal sys_* handler, or to sys_ni_syscall() which
 * returns -ENOSYS for unimplemented or stub syscalls.
 *
 * The table is referenced from syscall_asm.asm (syscall_linux_entry) and
 * from kpti_asm.asm when the Linux ABI is active.
 */

#include "syscall.h"
#include "module.h"

/* Module metadata */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Linux x86-64 ABI syscall dispatch table — sys_call_table[]");
MODULE_AUTHOR("Ruslan Gustomiasov");

/*
 * linux_syscall_t — type of a Linux-compatible syscall handler
 *
 * All Linux x86-64 syscalls receive up to 6 arguments via registers
 * (rdi, rsi, rdx, r10, r8, r9).  The handler returns a uint64_t result
 * which, for error cases, is a negative errno value (Linux convention).
 */
typedef uint64_t (*linux_syscall_t)(uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);

/*
 * sys_ni_syscall — placeholder for unimplemented syscalls
 *
 * Returns -ENOSYS to inform the caller that this syscall number is not
 * implemented.  Logging can be added per-subsystem as needed.
 */
static uint64_t sys_ni_syscall(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return (uint64_t)(int64_t)-ENOSYS;
}

/* ── Wrapper helper: discard unused args ─────────────────────── */

static inline void lin_discard6(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
}

static inline void lin_discard5(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
}

static inline void lin_discard4(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
}

static inline void lin_discard3(uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)a1; (void)a2; (void)a3;
}

static inline void lin_discard2(uint64_t a1, uint64_t a2)
{
    (void)a1; (void)a2;
}

static inline void lin_discard1(uint64_t a1)
{
    (void)a1;
}

/* ── Linux ABI wrapper functions ─────────────────────────────── */

static uint64_t lin_read(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_READ, a1, a2, a3, 0, 0);
}

static uint64_t lin_write(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_WRITE, a1, a2, a3, 0, 0);
}

static uint64_t lin_open(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_OPEN, a1, a2, a3, 0, 0);
}

static uint64_t lin_close(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_CLOSE, a1, 0, 0, 0, 0);
}

static uint64_t lin_stat(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_STAT, a1, a2, 0, 0, 0);
}

static uint64_t lin_lseek(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_LSEEK, a1, a2, a3, 0, 0);
}

static uint64_t lin_mmap(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    return syscall_dispatch_internal(SYS_MMAP, a1, a2, a3, a4, a5);
}

static uint64_t lin_mprotect(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_MPROTECT, a1, a2, a3, 0, 0);
}

static uint64_t lin_munmap(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_MUNMAP, a1, a2, 0, 0, 0);
}

static uint64_t lin_brk(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_BRK, a1, 0, 0, 0, 0);
}

static uint64_t lin_ioctl(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_IOCTL, a1, a2, a3, 0, 0);
}

static uint64_t lin_pread64(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_FD_READ, a1, a2, a3, 0, 0);
}

static uint64_t lin_pwrite64(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_FD_WRITE, a1, a2, a3, 0, 0);
}

static uint64_t lin_readv(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_READV, a1, a2, a3, 0, 0);
}

static uint64_t lin_writev(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_WRITEV, a1, a2, a3, 0, 0);
}

static uint64_t lin_access(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_ACCESS, a1, a2, 0, 0, 0);
}

static uint64_t lin_pipe(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_PIPE, a1, 0, 0, 0, 0);
}

static uint64_t lin_select(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_SELECT, a1, a2, a3, a4, a5);
}

static uint64_t lin_sched_yield(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_YIELD, 0, 0, 0, 0, 0);
}

static uint64_t lin_mremap(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard1(a6);
    return syscall_dispatch_internal(SYS_MREMAP, a1, a2, a3, a4, a5);
}

static uint64_t lin_msync(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_MSYNC, a1, a2, a3, 0, 0);
}

static uint64_t lin_mincore(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_MINCORE, a1, a2, a3, 0, 0);
}

static uint64_t lin_madvise(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_MADVISE, a1, a2, a3, 0, 0);
}

static uint64_t lin_shmget(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SHM_GET, a1, a2, a3, 0, 0);
}

static uint64_t lin_shmat(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SHM_AT, a1, a2, a3, 0, 0);
}

static uint64_t lin_shmctl(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SHMCTL, a1, a2, a3, 0, 0);
}

static uint64_t lin_dup(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_DUP, a1, 0, 0, 0, 0);
}

static uint64_t lin_dup2(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_DUP2, a1, a2, 0, 0, 0);
}

static uint64_t lin_pause(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_PAUSE, 0, 0, 0, 0, 0);
}

static uint64_t lin_nanosleep(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_NANOSLEEP, a1, a2, 0, 0, 0);
}

static uint64_t lin_getitimer(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETITIMER, a1, a2, 0, 0, 0);
}

static uint64_t lin_alarm(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_ALARM, a1, 0, 0, 0, 0);
}

static uint64_t lin_setitimer(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SETITIMER, a1, a2, a3, 0, 0);
}

static uint64_t lin_getpid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETPID, 0, 0, 0, 0, 0);
}

static uint64_t lin_sendfile(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_SENDFILE, a1, a2, a3, a4, 0);
}

static uint64_t lin_socket(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SOCKET, a1, a2, a3, 0, 0);
}

static uint64_t lin_connect(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_CONNECT, a1, a2, a3, 0, 0);
}

static uint64_t lin_accept(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_ACCEPT, a1, a2, a3, 0, 0);
}

static uint64_t lin_sendmsg(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SENDMSG, a1, a2, a3, 0, 0);
}

static uint64_t lin_recvmsg(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_RECVMSG, a1, a2, a3, 0, 0);
}

static uint64_t lin_bind(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_BIND, a1, a2, a3, 0, 0);
}

static uint64_t lin_listen(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_LISTEN, a1, a2, 0, 0, 0);
}

static uint64_t lin_getsockname(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETSOCKNAME, a1, a2, a3, 0, 0);
}

static uint64_t lin_getpeername(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETPEERNAME, a1, a2, a3, 0, 0);
}

static uint64_t lin_socketpair(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_SOCKETPAIR, a1, a2, a3, a4, 0);
}

static uint64_t lin_setsockopt(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_SETSOCKOPT, a1, a2, a3, a4, a5);
}

static uint64_t lin_getsockopt(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_GETSOCKOPT, a1, a2, a3, a4, a5);
}

static uint64_t lin_clone(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_CLONE, a1, a2, a3, a4, a5);
}

static uint64_t lin_fork(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_FORK, 0, 0, 0, 0, 0);
}

static uint64_t lin_execve(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_EXECVE, a1, a2, a3, 0, 0);
}

static uint64_t lin_exit(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_EXIT, a1, 0, 0, 0, 0);
}

static uint64_t lin_wait4(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_WAITPID, a1, a2, 0, 0, 0);
}

static uint64_t lin_kill(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_KILL, a1, a2, 0, 0, 0);
}

static uint64_t lin_uname(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_UNAME, a1, 0, 0, 0, 0);
}

static uint64_t lin_fcntl(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_FCNTL, a1, a2, a3, 0, 0);
}

static uint64_t lin_fsync(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_FSYNC, a1, 0, 0, 0, 0);
}

static uint64_t lin_fdatasync(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_FDATASYNC, a1, 0, 0, 0, 0);
}

static uint64_t lin_truncate(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_TRUNCATE, a1, a2, 0, 0, 0);
}

static uint64_t lin_ftruncate(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_FTRUNCATE, a1, a2, 0, 0, 0);
}

static uint64_t lin_getdents(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETDENTS64, a1, a2, a3, 0, 0);
}

static uint64_t lin_getcwd(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETCWD, a1, a2, 0, 0, 0);
}

static uint64_t lin_chdir(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_CHDIR, a1, 0, 0, 0, 0);
}

static uint64_t lin_rename(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_RENAME, a1, a2, 0, 0, 0);
}

static uint64_t lin_mkdir(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_MKDIR, a1, 0, 0, 0, 0);
}

static uint64_t lin_rmdir(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_RMDIR, a1, 0, 0, 0, 0);
}

static uint64_t lin_unlink(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_UNLINK, a1, 0, 0, 0, 0);
}

static uint64_t lin_chmod(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_CHMOD, a1, a2, 0, 0, 0);
}

static uint64_t lin_umask(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_UMASK, a1, 0, 0, 0, 0);
}

static uint64_t lin_getrusage(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETRUSAGE, a1, a2, 0, 0, 0);
}

static uint64_t lin_sysinfo(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SYSINFO, a1, 0, 0, 0, 0);
}

static uint64_t lin_getuid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETUID, 0, 0, 0, 0, 0);
}

static uint64_t lin_syslog(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SYSLOG, a1, a2, a3, 0, 0);
}

static uint64_t lin_getgid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETGID, 0, 0, 0, 0, 0);
}

static uint64_t lin_geteuid(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETEUID, 0, 0, 0, 0, 0);
}

static uint64_t lin_getegid(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETEGID, 0, 0, 0, 0, 0);
}

static uint64_t lin_setpgid(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SETPGID, a1, a2, 0, 0, 0);
}

static uint64_t lin_getppid(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETPPID, 0, 0, 0, 0, 0);
}

static uint64_t lin_setsid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SETSID, 0, 0, 0, 0, 0);
}

static uint64_t lin_getresuid(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETRESUID, a1, a2, a3, 0, 0);
}

static uint64_t lin_setresuid(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SETRESUID, a1, a2, a3, 0, 0);
}

static uint64_t lin_getresgid(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETRESGID, a1, a2, a3, 0, 0);
}

static uint64_t lin_setresgid(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SETRESGID, a1, a2, a3, 0, 0);
}

static uint64_t lin_sigaltstack(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SIGALTSTACK, a1, a2, 0, 0, 0);
}

static uint64_t lin_mknod(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_MKNOD, a1, a2, a3, 0, 0);
}

static uint64_t lin_personality(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_PERSONALITY, a1, 0, 0, 0, 0);
}

static uint64_t lin_statfs(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_STATFS, a1, a2, 0, 0, 0);
}

static uint64_t lin_fstatfs(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_FSTATFS, a1, a2, 0, 0, 0);
}

static uint64_t lin_getpriority(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETPRIORITY, a1, a2, 0, 0, 0);
}

static uint64_t lin_setpriority(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SETPRIORITY, a1, a2, a3, 0, 0);
}

static uint64_t lin_sched_setscheduler(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SCHED_SETSCHEDULER, a1, a2, a3, 0, 0);
}

static uint64_t lin_sched_getscheduler(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SCHED_GETSCHEDULER, a1, 0, 0, 0, 0);
}

static uint64_t lin_mlock(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_MLOCK, a1, a2, 0, 0, 0);
}

static uint64_t lin_munlock(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_MUNLOCK, a1, a2, 0, 0, 0);
}

static uint64_t lin_mlockall(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_MLOCKALL, a1, 0, 0, 0, 0);
}

static uint64_t lin_munlockall(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_MUNLOCKALL, 0, 0, 0, 0, 0);
}

static uint64_t lin_pivot_root(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_PIVOT_ROOT, a1, a2, 0, 0, 0);
}

static uint64_t lin_prctl(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_PRCTL, a1, a2, a3, a4, a5);
}

static uint64_t lin_arch_prctl(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_ARCH_PRCTL, a1, a2, 0, 0, 0);
}

static uint64_t lin_chroot(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_CHROOT, a1, 0, 0, 0, 0);
}

static uint64_t lin_sync(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SYNC, 0, 0, 0, 0, 0);
}

static uint64_t lin_mount(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_MOUNT, a1, a2, a3, a4, a5);
}

static uint64_t lin_umount2(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_UMOUNT, a1, 0, 0, 0, 0);
}

static uint64_t lin_reboot(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_REBOOT, 0, 0, 0, 0, 0);
}

static uint64_t lin_sethostname(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SETHOSTNAME, a1, a2, 0, 0, 0);
}

static uint64_t lin_sched_setaffinity(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SCHED_SETAFFINITY, a1, a2, 0, 0, 0);
}

static uint64_t lin_sched_getaffinity(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SCHED_GETAFFINITY, a1, 0, 0, 0, 0);
}

static uint64_t lin_gettid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETTID, 0, 0, 0, 0, 0);
}

static uint64_t lin_tkill(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_TKILL, a1, a2, 0, 0, 0);
}

static uint64_t lin_futex(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    return syscall_dispatch_internal(SYS_FUTEX, a1, a2, a3, a4, a5);
}

static uint64_t lin_set_robust_list(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SET_ROBUST_LIST, a1, a2, 0, 0, 0);
}

static uint64_t lin_get_robust_list(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_GET_ROBUST_LIST, a1, a2, a3, 0, 0);
}

static uint64_t lin_splice(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_SPLICE, a1, a2, a3, a4, a5);
}

static uint64_t lin_tee(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_TEE, a1, a2, a3, a4, 0);
}

static uint64_t lin_openat(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_OPENAT, a1, a2, a3, a4, 0);
}

static uint64_t lin_mkdirat(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_MKDIRAT, a1, a2, a3, 0, 0);
}

static uint64_t lin_newfstatat(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_FSTATAT, a1, a2, a3, a4, 0);
}

static uint64_t lin_unlinkat(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_UNLINKAT, a1, a2, a3, 0, 0);
}

static uint64_t lin_renameat(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_RENAMEAT, a1, a2, a3, a4, 0);
}

static uint64_t lin_symlinkat(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SYMLINKAT, a1, a2, a3, 0, 0);
}

static uint64_t lin_readlinkat(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_READLINKAT, a1, a2, a3, a4, 0);
}

static uint64_t lin_pselect6(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    return syscall_dispatch_internal(SYS_PSELECT6, a1, a2, a3, a4, a5);
}

static uint64_t lin_ppoll(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_PPOLL, a1, a2, a3, a4, 0);
}

static uint64_t lin_unshare(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_UNSHARE, a1, 0, 0, 0, 0);
}

static uint64_t lin_setns(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SETNS, a1, a2, 0, 0, 0);
}

static uint64_t lin_utimensat(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_UTIMENSAT, a1, a2, a3, a4, 0);
}

static uint64_t lin_fallocate(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_FALLOCATE, a1, a2, a3, a4, 0);
}

static uint64_t lin_timerfd_create(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_TIMERFD_CREATE, a1, a2, 0, 0, 0);
}

static uint64_t lin_timerfd_settime(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_TIMERFD_SETTIME, a1, a2, a3, a4, 0);
}

static uint64_t lin_timerfd_gettime(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_TIMERFD_GETTIME, a1, a2, 0, 0, 0);
}

static uint64_t lin_signalfd(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SIGNALFD, a1, a2, a3, 0, 0);
}

static uint64_t lin_eventfd(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_EVENTFD, a1, a2, 0, 0, 0);
}

static uint64_t lin_epoll_create1(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_EPOLL_CREATE1, a1, 0, 0, 0, 0);
}

static uint64_t lin_epoll_ctl(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_EPOLL_CTL, a1, a2, a3, a4, 0);
}

static uint64_t lin_epoll_wait(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_EPOLL_WAIT, a1, a2, a3, a4, 0);
}

static uint64_t lin_epoll_pwait(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_EPOLL_PWAIT, a1, a2, a3, a4, a5);
}

static uint64_t lin_dup3(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_DUP3, a1, a2, a3, 0, 0);
}

static uint64_t lin_pipe2(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_PIPE2, a1, a2, 0, 0, 0);
}

static uint64_t lin_inotify_init1(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_INOTIFY_INIT1, a1, 0, 0, 0, 0);
}

static uint64_t lin_prlimit64(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_PRLIMIT64, a1, a2, a3, a4, 0);
}

static uint64_t lin_name_to_handle_at(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_NAME_TO_HANDLE_AT, a1, a2, a3, a4, a5);
}

static uint64_t lin_open_by_handle_at(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_OPEN_BY_HANDLE_AT, a1, a2, a3, 0, 0);
}

static uint64_t lin_syncfs(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SYNCFS, a1, 0, 0, 0, 0);
}

static uint64_t lin_sendmmsg(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_SENDMMSG, a1, a2, a3, a4, 0);
}

static uint64_t lin_recvmmsg(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_RECVMMSG, a1, a2, a3, a4, a5);
}

static uint64_t lin_getcpu(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETCPU, a1, a2, a3, 0, 0);
}

static uint64_t lin_finit_module(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_FINIT_MODULE, a1, a2, a3, 0, 0);
}

static uint64_t lin_seccomp(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SECCOMP, a1, a2, a3, 0, 0);
}

static uint64_t lin_getrandom(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETRANDOM, a1, a2, a3, 0, 0);
}

static uint64_t lin_memfd_create(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_MEMFD_CREATE, a1, a2, 0, 0, 0);
}

static uint64_t lin_execveat(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_EXECVEAT, a1, a2, a3, a4, a5);
}

static uint64_t lin_userfaultfd(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_USERFAULTFD, a1, 0, 0, 0, 0);
}

static uint64_t lin_membarrier(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_MEMBARRIER, a1, a2, a3, 0, 0);
}

static uint64_t lin_copy_file_range(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    return syscall_dispatch_internal(SYS_COPY_FILE_RANGE, a1, a2, a3, a4, a5);
}

static uint64_t lin_rseq(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_RSEQ, a1, a2, a3, a4, 0);
}

static uint64_t lin_readahead(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_READAHEAD, a1, a2, a3, 0, 0);
}

static uint64_t lin_accept4(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_ACCEPT, a1, a2, a3, 0, 0);
}

static uint64_t lin_signalfd4(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SIGNALFD, a1, a2, a3, 0, 0);
}

static uint64_t lin_eventfd2(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_EVENTFD, a1, a2, 0, 0, 0);
}

static uint64_t lin_poll(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_POLL, a1, a2, a3, 0, 0);
}

static uint64_t lin_getdents64(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_GETDENTS64, a1, a2, a3, 0, 0);
}

static uint64_t lin_clock_gettime(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_CLOCK_GETTIME, a1, a2, 0, 0, 0);
}

static uint64_t lin_clock_settime(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_CLOCK_SETTIME, a1, a2, 0, 0, 0);
}

static uint64_t lin_clock_getres(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_CLOCK_GETRES, a1, a2, 0, 0, 0);
}

static uint64_t lin_timer_create(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_TIMER_CREATE, a1, a2, a3, 0, 0);
}

static uint64_t lin_timer_settime(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_TIMER_SETTIME, a1, a2, a3, a4, 0);
}

static uint64_t lin_timer_gettime(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_TIMER_GETTIME, a1, a2, 0, 0, 0);
}

static uint64_t lin_timer_getoverrun(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_TIMER_GETOVERRUN, a1, 0, 0, 0, 0);
}

static uint64_t lin_timer_delete(uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_TIMER_DELETE, a1, 0, 0, 0, 0);
}

static uint64_t lin_timerfd_create2(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_TIMERFD_CREATE, a1, a2, 0, 0, 0);
}

static uint64_t lin_sched_setattr(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_SCHED_SETATTR, a1, a2, a3, 0, 0);
}

static uint64_t lin_sched_getattr(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_SCHED_GETATTR, a1, a2, a3, a4, 0);
}

static uint64_t lin_shmdt(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard5(a2, a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_SHM_DT, a1, 0, 0, 0, 0);
}

static uint64_t lin_capget(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_CAPGET, a1, a2, 0, 0, 0);
}

static uint64_t lin_capset(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_CAPSET, a1, a2, 0, 0, 0);
}

static uint64_t lin_ioprio_set(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_IOPRIO_SET, a1, a2, a3, 0, 0);
}

static uint64_t lin_ioprio_get(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_IOPRIO_GET, a1, a2, 0, 0, 0);
}

static uint64_t lin_pidfd_open(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_PIDFD_OPEN, a1, a2, 0, 0, 0);
}

static uint64_t lin_pidfd_send_signal(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_PIDFD_SEND_SIGNAL, a1, a2, a3, 0, 0);
}

static uint64_t lin_pidfd_getfd(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_PIDFD_GETFD, a1, a2, a3, 0, 0);
}

static uint64_t lin_close_range(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_CLOSE_RANGE, a1, a2, a3, 0, 0);
}

static uint64_t lin_io_uring_setup(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard4(a3, a4, a5, a6);
    return syscall_dispatch_internal(SYS_IO_URING_SETUP, a1, a2, 0, 0, 0);
}

static uint64_t lin_io_uring_enter(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_IO_URING_ENTER, a1, a2, a3, a4, 0);
}

static uint64_t lin_io_uring_register(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard2(a5, a6);
    return syscall_dispatch_internal(SYS_IO_URING_REGISTER, a1, a2, a3, a4, 0);
}

static uint64_t lin_mount_setattr(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a6;
    return syscall_dispatch_internal(SYS_MOUNT_SETATTR, a1, a2, a3, a4, a5);
}

static uint64_t lin_pkey_mprotect(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard3(a4, a5, a6);
    return syscall_dispatch_internal(SYS_MPROTECT, a1, a2, a3, 0, 0);
}

static uint64_t lin_mbind(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    lin_discard6(a1, a2, a3, a4, a5, a6);
    return (uint64_t)(int64_t)-ENOSYS;
}

/*
 * sys_call_table[] — Linux x86-64 syscall dispatch table
 *
 * Indexed by Linux __NR_* syscall numbers (0 through __NR_syscalls-1).
 * Each entry points to the appropriate Linux-compatible handler or to
 * sys_ni_syscall for unimplemented syscalls.
 *
 * Entries are explicitly ordered by __NR number so that assembly-level
 * dispatch via syscall_linux_entry can index directly.
 */
__attribute__((used))
linux_syscall_t sys_call_table[__NR_syscalls] = {
    [0]   = lin_read,            /* __NR_read */
    [1]   = lin_write,           /* __NR_write */
    [2]   = lin_open,            /* __NR_open */
    [3]   = lin_close,           /* __NR_close */
    [4]   = lin_stat,            /* __NR_stat */
    [5]   = sys_ni_syscall,     /* __NR_fstat (no internal equivalent) */
    [6]   = sys_ni_syscall,     /* __NR_lstat (no internal equivalent) */
    [7]   = lin_poll,            /* __NR_poll */
    [8]   = lin_lseek,           /* __NR_lseek */
    [9]   = lin_mmap,            /* __NR_mmap */
    [10]  = lin_mprotect,        /* __NR_mprotect */
    [11]  = lin_munmap,          /* __NR_munmap */
    [12]  = lin_brk,             /* __NR_brk */
    [13]  = sys_ni_syscall,     /* __NR_rt_sigaction (no internal equiv) */
    [14]  = sys_ni_syscall,     /* __NR_rt_sigprocmask (no internal equiv) */
    [15]  = sys_ni_syscall,     /* __NR_rt_sigreturn (no internal equiv) */
    [16]  = lin_ioctl,           /* __NR_ioctl */
    [17]  = lin_pread64,         /* __NR_pread64 */
    [18]  = lin_pwrite64,        /* __NR_pwrite64 */
    [19]  = lin_readv,           /* __NR_readv */
    [20]  = lin_writev,          /* __NR_writev */
    [21]  = lin_access,          /* __NR_access */
    [22]  = lin_pipe,            /* __NR_pipe */
    [23]  = lin_select,          /* __NR_select */
    [24]  = lin_sched_yield,     /* __NR_sched_yield */
    [25]  = lin_mremap,          /* __NR_mremap */
    [26]  = lin_msync,           /* __NR_msync */
    [27]  = lin_mincore,         /* __NR_mincore */
    [28]  = lin_madvise,         /* __NR_madvise */
    [29]  = lin_shmget,          /* __NR_shmget */
    [30]  = lin_shmat,           /* __NR_shmat */
    [31]  = lin_shmctl,          /* __NR_shmctl */
    [32]  = lin_dup,             /* __NR_dup */
    [33]  = lin_dup2,            /* __NR_dup2 */
    [34]  = lin_pause,           /* __NR_pause */
    [35]  = lin_nanosleep,       /* __NR_nanosleep */
    [36]  = lin_getitimer,       /* __NR_getitimer */
    [37]  = lin_alarm,           /* __NR_alarm */
    [38]  = lin_setitimer,       /* __NR_setitimer */
    [39]  = lin_getpid,          /* __NR_getpid */
    [40]  = lin_sendfile,        /* __NR_sendfile */
    [41]  = lin_socket,          /* __NR_socket */
    [42]  = lin_connect,         /* __NR_connect */
    [43]  = lin_accept,          /* __NR_accept */
    [44]  = sys_ni_syscall,     /* __NR_sendto (no internal equivalent) */
    [45]  = sys_ni_syscall,     /* __NR_recvfrom (no internal equivalent) */
    [46]  = lin_sendmsg,         /* __NR_sendmsg */
    [47]  = lin_recvmsg,         /* __NR_recvmsg */
    [48]  = sys_ni_syscall,     /* __NR_shutdown (no internal equivalent) */
    [49]  = lin_bind,            /* __NR_bind */
    [50]  = lin_listen,          /* __NR_listen */
    [51]  = lin_getsockname,     /* __NR_getsockname */
    [52]  = lin_getpeername,     /* __NR_getpeername */
    [53]  = lin_socketpair,      /* __NR_socketpair */
    [54]  = lin_setsockopt,      /* __NR_setsockopt */
    [55]  = lin_getsockopt,      /* __NR_getsockopt */
    [56]  = lin_clone,           /* __NR_clone */
    [57]  = lin_fork,            /* __NR_fork */
    [58]  = sys_ni_syscall,     /* __NR_vfork (no internal equivalent) */
    [59]  = lin_execve,          /* __NR_execve */
    [60]  = lin_exit,            /* __NR_exit */
    [61]  = lin_wait4,           /* __NR_wait4 */
    [62]  = lin_kill,            /* __NR_kill */
    [63]  = lin_uname,           /* __NR_uname */
    [64]  = sys_ni_syscall,     /* __NR_semget (no internal equiv) */
    [65]  = sys_ni_syscall,     /* __NR_semop (no internal equiv) */
    [66]  = sys_ni_syscall,     /* __NR_semctl (no internal equiv) */
    [67]  = lin_shmdt,           /* __NR_shmdt */
    [68]  = sys_ni_syscall,     /* __NR_msgget (no internal equiv) */
    [69]  = sys_ni_syscall,     /* __NR_msgsnd (no internal equiv) */
    [70]  = sys_ni_syscall,     /* __NR_msgrcv (no internal equiv) */
    [71]  = sys_ni_syscall,     /* __NR_msgctl (no internal equiv) */
    [72]  = lin_fcntl,           /* __NR_fcntl */
    [73]  = sys_ni_syscall,     /* __NR_flock (no internal equiv) */
    [74]  = lin_fsync,           /* __NR_fsync */
    [75]  = lin_fdatasync,       /* __NR_fdatasync */
    [76]  = lin_truncate,        /* __NR_truncate */
    [77]  = lin_ftruncate,       /* __NR_ftruncate */
    [78]  = lin_getdents,        /* __NR_getdents */
    [79]  = lin_getcwd,          /* __NR_getcwd */
    [80]  = lin_chdir,           /* __NR_chdir */
    [81]  = sys_ni_syscall,     /* __NR_fchdir (no internal equiv) */
    [82]  = lin_rename,          /* __NR_rename */
    [83]  = lin_mkdir,           /* __NR_mkdir */
    [84]  = lin_rmdir,           /* __NR_rmdir */
    [85]  = sys_ni_syscall,     /* __NR_creat (no internal equiv) */
    [86]  = sys_ni_syscall,     /* __NR_link (no internal equiv) */
    [87]  = lin_unlink,          /* __NR_unlink */
    [88]  = sys_ni_syscall,     /* __NR_symlink (no internal equiv) */
    [89]  = sys_ni_syscall,     /* __NR_readlink (no internal equiv) */
    [90]  = lin_chmod,           /* __NR_chmod */
    [91]  = sys_ni_syscall,     /* __NR_fchmod (no internal equiv) */
    [92]  = sys_ni_syscall,     /* __NR_chown (no internal equiv) */
    [93]  = sys_ni_syscall,     /* __NR_fchown (no internal equiv) */
    [94]  = sys_ni_syscall,     /* __NR_lchown (no internal equiv) */
    [95]  = lin_umask,           /* __NR_umask */
    [96]  = sys_ni_syscall,     /* __NR_gettimeofday (no internal equiv) */
    [97]  = sys_ni_syscall,     /* __NR_getrlimit (no internal equiv) */
    [98]  = lin_getrusage,       /* __NR_getrusage */
    [99]  = lin_sysinfo,         /* __NR_sysinfo */
    [100] = sys_ni_syscall,     /* __NR_times (no internal equiv) */
    [101] = sys_ni_syscall,     /* __NR_ptrace (no internal equiv) */
    [102] = lin_getuid,          /* __NR_getuid */
    [103] = lin_syslog,          /* __NR_syslog */
    [104] = lin_getgid,          /* __NR_getgid */
    [105] = sys_ni_syscall,     /* __NR_setuid (no internal equiv) */
    [106] = sys_ni_syscall,     /* __NR_setgid (no internal equiv) */
    [107] = lin_geteuid,         /* __NR_geteuid */
    [108] = lin_getegid,         /* __NR_getegid */
    [109] = lin_setpgid,         /* __NR_setpgid */
    [110] = lin_getppid,         /* __NR_getppid */
    [111] = sys_ni_syscall,     /* __NR_getpgrp (no internal equiv) */
    [112] = lin_setsid,          /* __NR_setsid */
    [113] = sys_ni_syscall,     /* __NR_setreuid (no internal equiv) */
    [114] = sys_ni_syscall,     /* __NR_setregid (no internal equiv) */
    [115] = sys_ni_syscall,     /* __NR_getgroups (no internal equiv) */
    [116] = sys_ni_syscall,     /* __NR_setgroups (no internal equiv) */
    [117] = lin_setresuid,       /* __NR_setresuid */
    [118] = lin_getresuid,       /* __NR_getresuid */
    [119] = lin_setresgid,       /* __NR_setresgid */
    [120] = lin_getresgid,       /* __NR_getresgid */
    [121] = sys_ni_syscall,     /* __NR_getpgid (no internal equiv) */
    [122] = sys_ni_syscall,     /* __NR_setfsuid (no internal equiv) */
    [123] = sys_ni_syscall,     /* __NR_setfsgid (no internal equiv) */
    [124] = sys_ni_syscall,     /* __NR_getsid (no internal equiv) */
    [125] = lin_capget,          /* __NR_capget */
    [126] = lin_capset,          /* __NR_capset */
    [127] = sys_ni_syscall,     /* __NR_rt_sigpending */
    [128] = sys_ni_syscall,     /* __NR_rt_sigtimedwait */
    [129] = sys_ni_syscall,     /* __NR_rt_sigqueueinfo */
    [130] = sys_ni_syscall,     /* __NR_rt_sigsuspend */
    [131] = lin_sigaltstack,     /* __NR_sigaltstack */
    [132] = sys_ni_syscall,     /* __NR_utime */
    [133] = lin_mknod,           /* __NR_mknod */
    [134] = sys_ni_syscall,     /* __NR_uselib */
    [135] = lin_personality,     /* __NR_personality */
    [136] = sys_ni_syscall,     /* __NR_ustat */
    [137] = lin_statfs,          /* __NR_statfs */
    [138] = lin_fstatfs,         /* __NR_fstatfs */
    [139] = sys_ni_syscall,     /* __NR_sysfs */
    [140] = lin_getpriority,     /* __NR_getpriority */
    [141] = lin_setpriority,     /* __NR_setpriority */
    [142] = sys_ni_syscall,     /* __NR_sched_setparam */
    [143] = sys_ni_syscall,     /* __NR_sched_getparam */
    [144] = lin_sched_setscheduler, /* __NR_sched_setscheduler */
    [145] = lin_sched_getscheduler, /* __NR_sched_getscheduler */
    [146] = sys_ni_syscall,     /* __NR_sched_get_priority_max */
    [147] = sys_ni_syscall,     /* __NR_sched_get_priority_min */
    [148] = sys_ni_syscall,     /* __NR_sched_rr_get_interval */
    [149] = lin_mlock,           /* __NR_mlock */
    [150] = lin_munlock,         /* __NR_munlock */
    [151] = lin_mlockall,        /* __NR_mlockall */
    [152] = lin_munlockall,      /* __NR_munlockall */
    [153] = sys_ni_syscall,     /* __NR_vhangup */
    [154] = sys_ni_syscall,     /* __NR_modify_ldt */
    [155] = lin_pivot_root,      /* __NR_pivot_root */
    [156] = sys_ni_syscall,     /* __NR__sysctl */
    [157] = lin_prctl,           /* __NR_prctl */
    [158] = lin_arch_prctl,      /* __NR_arch_prctl */
    [159] = sys_ni_syscall,     /* __NR_adjtimex */
    [160] = sys_ni_syscall,     /* __NR_setrlimit */
    [161] = lin_chroot,          /* __NR_chroot */
    [162] = lin_sync,            /* __NR_sync */
    [163] = sys_ni_syscall,     /* __NR_acct */
    [164] = sys_ni_syscall,     /* __NR_settimeofday */
    [165] = lin_mount,           /* __NR_mount */
    [166] = lin_umount2,         /* __NR_umount2 */
    [167] = sys_ni_syscall,     /* __NR_swapon */
    [168] = sys_ni_syscall,     /* __NR_swapoff */
    [169] = lin_reboot,          /* __NR_reboot */
    [170] = lin_sethostname,     /* __NR_sethostname */
    [171] = sys_ni_syscall,     /* __NR_setdomainname */
    [172] = sys_ni_syscall,     /* __NR_iopl */
    [173] = sys_ni_syscall,     /* __NR_ioperm */
    [174] = sys_ni_syscall,     /* __NR_create_module */
    [175] = sys_ni_syscall,     /* __NR_init_module (use finit_module) */
    [176] = sys_ni_syscall,     /* __NR_delete_module */
    [177] = sys_ni_syscall,     /* __NR_get_kernel_syms */
    [178] = sys_ni_syscall,     /* __NR_query_module */
    [179] = sys_ni_syscall,     /* __NR_quotactl */
    [180] = sys_ni_syscall,     /* __NR_nfsservctl */
    [181] = sys_ni_syscall,     /* __NR_getpmsg */
    [182] = sys_ni_syscall,     /* __NR_putpmsg */
    [183] = sys_ni_syscall,     /* __NR_afs_syscall */
    [184] = sys_ni_syscall,     /* __NR_tuxcall */
    [185] = sys_ni_syscall,     /* __NR_security */
    [186] = lin_gettid,          /* __NR_gettid */
    [187] = lin_readahead,       /* __NR_readahead */
    [188] = sys_ni_syscall,     /* __NR_setxattr */
    [189] = sys_ni_syscall,     /* __NR_lsetxattr */
    [190] = sys_ni_syscall,     /* __NR_fsetxattr */
    [191] = sys_ni_syscall,     /* __NR_getxattr */
    [192] = sys_ni_syscall,     /* __NR_lgetxattr */
    [193] = sys_ni_syscall,     /* __NR_fgetxattr */
    [194] = sys_ni_syscall,     /* __NR_listxattr */
    [195] = sys_ni_syscall,     /* __NR_llistxattr */
    [196] = sys_ni_syscall,     /* __NR_flistxattr */
    [197] = sys_ni_syscall,     /* __NR_removexattr */
    [198] = sys_ni_syscall,     /* __NR_lremovexattr */
    [199] = sys_ni_syscall,     /* __NR_fremovexattr */
    [200] = lin_tkill,           /* __NR_tkill */
    [201] = sys_ni_syscall,     /* __NR_time */
    [202] = lin_futex,           /* __NR_futex */
    [203] = lin_sched_setaffinity,  /* __NR_sched_setaffinity */
    [204] = lin_sched_getaffinity,  /* __NR_sched_getaffinity */
    [205] = sys_ni_syscall,     /* __NR_set_thread_area */
    [206] = sys_ni_syscall,     /* __NR_io_setup */
    [207] = sys_ni_syscall,     /* __NR_io_destroy */
    [208] = sys_ni_syscall,     /* __NR_io_getevents */
    [209] = sys_ni_syscall,     /* __NR_io_submit */
    [210] = sys_ni_syscall,     /* __NR_io_cancel */
    [211] = sys_ni_syscall,     /* __NR_get_thread_area */
    [212] = sys_ni_syscall,     /* __NR_lookup_dcookie */
    [213] = sys_ni_syscall,     /* __NR_epoll_create (use epoll_create1) */
    [214] = sys_ni_syscall,     /* __NR_epoll_ctl_old */
    [215] = sys_ni_syscall,     /* __NR_epoll_wait_old */
    [216] = sys_ni_syscall,     /* __NR_remap_file_pages */
    [217] = lin_getdents64,      /* __NR_getdents64 */
    [218] = sys_ni_syscall,     /* __NR_set_tid_address */
    [219] = sys_ni_syscall,     /* __NR_restart_syscall */
    [220] = sys_ni_syscall,     /* __NR_semtimedop */
    [221] = sys_ni_syscall,     /* __NR_fadvise64 */
    [222] = lin_timer_create,    /* __NR_timer_create */
    [223] = lin_timer_settime,   /* __NR_timer_settime */
    [224] = lin_timer_gettime,   /* __NR_timer_gettime */
    [225] = lin_timer_getoverrun, /* __NR_timer_getoverrun */
    [226] = lin_timer_delete,    /* __NR_timer_delete */
    [227] = lin_clock_settime,   /* __NR_clock_settime */
    [228] = lin_clock_gettime,   /* __NR_clock_gettime */
    [229] = lin_clock_getres,    /* __NR_clock_getres */
    [230] = sys_ni_syscall,     /* __NR_clock_nanosleep */
    [231] = sys_ni_syscall,     /* __NR_exit_group */
    [232] = lin_epoll_wait,      /* __NR_epoll_wait */
    [233] = lin_epoll_ctl,       /* __NR_epoll_ctl */
    [234] = sys_ni_syscall,     /* __NR_tgkill */
    [235] = sys_ni_syscall,     /* __NR_utimes */
    [236] = sys_ni_syscall,     /* __NR_vserver */
    [237] = lin_mbind,           /* __NR_mbind */
    [238] = sys_ni_syscall,     /* __NR_set_mempolicy */
    [239] = sys_ni_syscall,     /* __NR_get_mempolicy */
    [240] = sys_ni_syscall,     /* __NR_mq_open */
    [241] = sys_ni_syscall,     /* __NR_mq_unlink */
    [242] = sys_ni_syscall,     /* __NR_mq_timedsend */
    [243] = sys_ni_syscall,     /* __NR_mq_timedreceive */
    [244] = sys_ni_syscall,     /* __NR_mq_notify */
    [245] = sys_ni_syscall,     /* __NR_mq_getsetattr */
    [246] = sys_ni_syscall,     /* __NR_kexec_load */
    [247] = sys_ni_syscall,     /* __NR_waitid */
    [248] = sys_ni_syscall,     /* __NR_add_key */
    [249] = sys_ni_syscall,     /* __NR_request_key */
    [250] = sys_ni_syscall,     /* __NR_keyctl */
    [251] = lin_ioprio_set,      /* __NR_ioprio_set */
    [252] = lin_ioprio_get,      /* __NR_ioprio_get */
    [253] = sys_ni_syscall,     /* __NR_inotify_init (use inotify_init1) */
    [254] = sys_ni_syscall,     /* __NR_inotify_add_watch */
    [255] = sys_ni_syscall,     /* __NR_inotify_rm_watch */
    [256] = sys_ni_syscall,     /* __NR_migrate_pages */
    [257] = lin_openat,          /* __NR_openat */
    [258] = lin_mkdirat,         /* __NR_mkdirat */
    [259] = sys_ni_syscall,     /* __NR_mknodat */
    [260] = sys_ni_syscall,     /* __NR_fchownat */
    [261] = sys_ni_syscall,     /* __NR_futimesat */
    [262] = lin_newfstatat,      /* __NR_newfstatat */
    [263] = lin_unlinkat,        /* __NR_unlinkat */
    [264] = lin_renameat,        /* __NR_renameat */
    [265] = sys_ni_syscall,     /* __NR_linkat */
    [266] = lin_symlinkat,       /* __NR_symlinkat */
    [267] = lin_readlinkat,      /* __NR_readlinkat */
    [268] = sys_ni_syscall,     /* __NR_fchmodat */
    [269] = sys_ni_syscall,     /* __NR_faccessat */
    [270] = lin_pselect6,        /* __NR_pselect6 */
    [271] = lin_ppoll,           /* __NR_ppoll */
    [272] = lin_unshare,         /* __NR_unshare */
    [273] = lin_set_robust_list, /* __NR_set_robust_list */
    [274] = lin_get_robust_list, /* __NR_get_robust_list */
    [275] = lin_splice,          /* __NR_splice */
    [276] = lin_tee,             /* __NR_tee */
    [277] = sys_ni_syscall,     /* __NR_sync_file_range */
    [278] = sys_ni_syscall,     /* __NR_vmsplice */
    [279] = sys_ni_syscall,     /* __NR_move_pages */
    [280] = lin_utimensat,       /* __NR_utimensat */
    [281] = lin_epoll_pwait,     /* __NR_epoll_pwait */
    [282] = lin_signalfd,        /* __NR_signalfd */
    [283] = lin_timerfd_create,  /* __NR_timerfd_create */
    [284] = lin_eventfd,         /* __NR_eventfd */
    [285] = lin_fallocate,       /* __NR_fallocate */
    [286] = lin_timerfd_settime, /* __NR_timerfd_settime */
    [287] = lin_timerfd_gettime, /* __NR_timerfd_gettime */
    [288] = lin_accept4,         /* __NR_accept4 */
    [289] = lin_signalfd4,       /* __NR_signalfd4 */
    [290] = lin_eventfd2,        /* __NR_eventfd2 */
    [291] = lin_epoll_create1,   /* __NR_epoll_create1 */
    [292] = lin_dup3,            /* __NR_dup3 */
    [293] = lin_pipe2,           /* __NR_pipe2 */
    [294] = lin_inotify_init1,   /* __NR_inotify_init1 */
    [295] = sys_ni_syscall,     /* __NR_preadv */
    [296] = sys_ni_syscall,     /* __NR_pwritev */
    [297] = sys_ni_syscall,     /* __NR_rt_tgsigqueueinfo */
    [298] = sys_ni_syscall,     /* __NR_perf_event_open */
    [299] = lin_recvmmsg,        /* __NR_recvmmsg */
    [300] = sys_ni_syscall,     /* __NR_fanotify_init */
    [301] = sys_ni_syscall,     /* __NR_fanotify_mark */
    [302] = lin_prlimit64,       /* __NR_prlimit64 */
    [303] = lin_name_to_handle_at,  /* __NR_name_to_handle_at */
    [304] = lin_open_by_handle_at,  /* __NR_open_by_handle_at */
    [305] = sys_ni_syscall,     /* __NR_clock_adjtime */
    [306] = lin_syncfs,          /* __NR_syncfs */
    [307] = lin_sendmmsg,        /* __NR_sendmmsg */
    [308] = lin_setns,           /* __NR_setns */
    [309] = lin_getcpu,          /* __NR_getcpu */
    [310] = sys_ni_syscall,     /* __NR_process_vm_readv */
    [311] = sys_ni_syscall,     /* __NR_process_vm_writev */
    [312] = sys_ni_syscall,     /* __NR_kcmp */
    [313] = lin_finit_module,    /* __NR_finit_module */
    [314] = lin_sched_setattr,   /* __NR_sched_setattr */
    [315] = lin_sched_getattr,   /* __NR_sched_getattr */
    [316] = sys_ni_syscall,     /* __NR_renameat2 (no internal equiv) */
    [317] = lin_seccomp,         /* __NR_seccomp */
    [318] = lin_getrandom,       /* __NR_getrandom */
    [319] = lin_memfd_create,    /* __NR_memfd_create */
    [320] = sys_ni_syscall,     /* __NR_kexec_file_load */
    [321] = sys_ni_syscall,     /* __NR_bpf */
    [322] = lin_execveat,        /* __NR_execveat */
    [323] = lin_userfaultfd,     /* __NR_userfaultfd */
    [324] = lin_membarrier,      /* __NR_membarrier */
    [325] = sys_ni_syscall,     /* __NR_mlock2 */
    [326] = lin_copy_file_range, /* __NR_copy_file_range */
    [327] = sys_ni_syscall,     /* __NR_preadv2 */
    [328] = sys_ni_syscall,     /* __NR_pwritev2 */
    [329] = lin_pkey_mprotect,   /* __NR_pkey_mprotect */
    [330] = sys_ni_syscall,     /* __NR_pkey_alloc */
    [331] = sys_ni_syscall,     /* __NR_pkey_free */
    [332] = sys_ni_syscall,     /* __NR_statx */
    [333] = sys_ni_syscall,     /* __NR_io_pgetevents */
    [334] = lin_rseq,            /* __NR_rseq */
};

/* System call argument 6 — saved by the asm entry before dispatch */
extern uint64_t syscall_arg6;

/*
 * syscall_linux_dispatch — dispatch a Linux ABI syscall via sys_call_table[]
 *
 * Called from the syscall_linux_entry asm wrapper (or from the KPTI trampoline
 * path via syscall_linux_entry_full).  Validates the syscall number, then
 * indexes sys_call_table[] and calls the appropriate handler with up to 6 args.
 *
 * The 6th argument is read from syscall_arg6 (saved by the asm entry path).
 * Returns the handler's uint64_t result directly — no seccomp/interposition.
 */
uint64_t syscall_linux_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t a6;
    uint64_t result;

    /* Bounds check: return -ENOSYS for out-of-range syscall numbers */
    if (num >= __NR_syscalls)
        return (uint64_t)(int64_t)-ENOSYS;

    /* Read the 6th argument saved by the asm entry */
    a6 = syscall_arg6;

    /* Dispatch via the Linux ABI table */
    result = sys_call_table[num](a1, a2, a3, a4, a5, a6);
    return result;
}
