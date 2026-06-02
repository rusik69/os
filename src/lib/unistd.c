/*
 * unistd.c — POSIX standard system call wrappers (Item U20)
 *
 * Implements the functions declared in <unistd.h> by calling the
 * kernel's syscall interface via libc_syscall().  Many of these
 * functions are already available in libc.c under different names;
 * this file provides the standard POSIX names and signatures.
 */

#include "unistd.h"
#include "libc.h"
#include "syscall.h"
#include "string.h"  /* for strcpy, etc. */
#include "stdlib.h"  /* for malloc */

/* ── Helper: raw syscall dispatcher ─────────────────────────────── */

static inline long do_syscall6(uint64_t num, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    return (long)libc_syscall(num, a1, a2, a3, a4, a5);
}

static inline long do_syscall(uint64_t num) {
    return do_syscall6(num, 0, 0, 0, 0, 0);
}

/* ── Process lifetime ───────────────────────────────────────────── */

int fork(void) {
    return libc_fork();
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    /* sys_execve takes path, argv, envp as user-space pointers.
     * The kernel currently ignores argv/envp but we pass them for
     * future compatibility. */
    return (int)do_syscall6(SYS_EXECVE,
                            (uint64_t)(uintptr_t)path,
                            (uint64_t)(uintptr_t)argv,
                            (uint64_t)(uintptr_t)envp,
                            0, 0);
}

int execvp(const char *file, char *const argv[]) {
    /* Search PATH if file does not contain a '/' */
    if (file && strchr(file, '/') == NULL) {
        const char *path_env = "/bin";  /* default search path */
        char full_path[256];
        const char *p = path_env;
        while (p && *p) {
            const char *next = strchr(p, ':');
            size_t seg_len = next ? (size_t)(next - p) : strlen(p);
            if (seg_len + 1 + strlen(file) + 1 > sizeof(full_path))
                break;
            memcpy(full_path, p, seg_len);
            full_path[seg_len] = '/';
            strcpy(full_path + seg_len + 1, file);
            full_path[seg_len + 1 + strlen(file)] = '\0';
            int ret = execve(full_path, argv, NULL);
            if (ret == 0) return 0;  /* should not reach here */
            p = next ? next + 1 : NULL;
        }
        return -1;  /* not found */
    }
    return execve(file, argv, NULL);
}

int execl(const char *path, const char *arg, ...) {
    /* For now, pass the path as the program and ignore additional args.
     * A full implementation would pack variadic args into an argv array. */
    (void)arg;
    return execve(path, NULL, NULL);
}

int execveat(int dirfd, const char *path, char *const argv[],
             char *const envp[], int flags) {
    return (int)do_syscall6(SYS_EXECVEAT,
                            (uint64_t)(int64_t)dirfd,
                            (uint64_t)(uintptr_t)path,
                            (uint64_t)(uintptr_t)argv,
                            (uint64_t)(uintptr_t)envp,
                            (uint64_t)flags);
}

void _exit(int status) {
    do_syscall6(SYS_EXIT, (uint64_t)(int64_t)status, 0, 0, 0, 0);
    for (;;) __asm__ volatile("hlt");  /* should not reach here */
}

unsigned int getpid(void) {
    return (unsigned int)libc_getpid();
}

unsigned int getppid(void) {
    return (unsigned int)do_syscall(SYS_GETPPID);
}

/* ── Process groups / sessions ──────────────────────────────────── */

int setpgid(unsigned int pid, unsigned int pgid) {
    return (int)do_syscall6(SYS_SETPGID,
                            (uint64_t)pid, (uint64_t)pgid, 0, 0, 0);
}

unsigned int getpgid(unsigned int pid) {
    return (unsigned int)do_syscall6(SYS_GETPGID, (uint64_t)pid, 0, 0, 0, 0);
}

unsigned int setsid(void) {
    return (unsigned int)do_syscall(SYS_SETSID);
}

unsigned int getsid(unsigned int pid) {
    return (unsigned int)do_syscall6(SYS_GETSID, (uint64_t)pid, 0, 0, 0, 0);
}

/* ── Signals ────────────────────────────────────────────────────── */

unsigned int alarm(unsigned int seconds) {
    return (unsigned int)do_syscall6(SYS_ALARM, (uint64_t)seconds, 0, 0, 0, 0);
}

int pause(void) {
    return (int)do_syscall(SYS_PAUSE);
}

/* ── File operations ────────────────────────────────────────────── */

int access(const char *path, int mode) {
    return (int)do_syscall6(SYS_ACCESS,
                            (uint64_t)(uintptr_t)path,
                            (uint64_t)(int64_t)mode,
                            0, 0, 0);
}

char *getcwd(char *buf, size_t size) {
    /* If buf is NULL, allocate a buffer of size */
    if (buf == NULL) {
        if (size == 0) size = 256;
        buf = (char *)libc_malloc(size);
        if (!buf) return NULL;
    }
    int ret = libc_getcwd(buf, (int)size);
    if (ret < 0) return NULL;
    return buf;
}

int chdir(const char *path) {
    return libc_chdir(path);
}

int dup(int oldfd) {
    return (int)do_syscall6(SYS_DUP, (uint64_t)(int64_t)oldfd, 0, 0, 0, 0);
}

int dup2(int oldfd, int newfd) {
    return (int)do_syscall6(SYS_DUP2,
                            (uint64_t)(int64_t)oldfd,
                            (uint64_t)(int64_t)newfd,
                            0, 0, 0);
}

int close(int fd) {
    return (int)do_syscall6(SYS_CLOSE, (uint64_t)(int64_t)fd, 0, 0, 0, 0);
}

ssize_t read(int fd, void *buf, size_t count) {
    return libc_fd_read(fd, buf, (uint32_t)count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return libc_fd_write(fd, buf, (uint32_t)count);
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)libc_lseek(fd, (int64_t)offset, whence);
}

/* ── Pipes ──────────────────────────────────────────────────────── */

int pipe(int pipefd[2]) {
    return (int)do_syscall6(SYS_PIPE,
                            (uint64_t)(uintptr_t)pipefd, 0, 0, 0, 0);
}

/* ── System configuration ───────────────────────────────────────── */

long sysconf(int name) {
    switch (name) {
        case _SC_PAGESIZE:          return 4096;
        case _SC_NPROCESSORS_CONF:  return 1;  /* default: single CPU */
        case _SC_NPROCESSORS_ONLN:  return 1;  /* default: single CPU */
        case _SC_PHYS_PAGES: {
            struct libc_pmm_stats stats;
            if (libc_pmm_get_stats(&stats) == 0)
                return (long)stats.total_pages;
            return -1;
        }
        case _SC_AVPHYS_PAGES: {
            struct libc_pmm_stats stats;
            if (libc_pmm_get_stats(&stats) == 0)
                return (long)stats.free_pages;
            return -1;
        }
        case _SC_CLK_TCK:           return 100;
        case _SC_OPEN_MAX:          return 256;
        case _SC_STREAM_MAX:        return 16;
        case _SC_TZNAME_MAX:        return 64;
        default:                    return -1;
    }
}

int gethostname(char *name, size_t len) {
    if (!name || len == 0) return -1;
    return (int)do_syscall6(SYS_GETHOSTNAME,
                            (uint64_t)(uintptr_t)name,
                            (uint64_t)len, 0, 0, 0);
}

int sethostname(const char *name, size_t len) {
    if (!name || len == 0) return -1;
    return (int)do_syscall6(SYS_SETHOSTNAME,
                            (uint64_t)(uintptr_t)name,
                            (uint64_t)len, 0, 0, 0);
}

/* ── Sleeping ───────────────────────────────────────────────────── */

unsigned int sleep(unsigned int seconds) {
    /* Convert seconds to nanoseconds and use nanosleep */
    struct timespec req;
    struct timespec rem;
    req.tv_sec = (int64_t)seconds;
    req.tv_nsec = 0;
    if (libc_nanosleep(&req, &rem) == 0)
        return 0;
    /* If interrupted, return remaining seconds (approximate) */
    return (unsigned int)rem.tv_sec + (rem.tv_nsec > 0 ? 1 : 0);
}

int usleep(unsigned long usec) {
    struct timespec req;
    struct timespec rem;
    req.tv_sec = (int64_t)(usec / 1000000UL);
    req.tv_nsec = (int64_t)((usec % 1000000UL) * 1000L);
    return libc_nanosleep(&req, &rem);
}

/* ── I/O control ────────────────────────────────────────────────── */

int ioctl(int fd, unsigned long request, ...) {
    /* For now, stub: no ioctl support beyond standard operations.
     * The kernel's SYS_IOCTL can be called here. */
    (void)fd;
    (void)request;
    return -1;  /* ENOTTY */
}

/* ── Truncation ─────────────────────────────────────────────────── */

int truncate(const char *path, off_t length) {
    return libc_truncate(path, (uint32_t)length);
}

int ftruncate(int fd, off_t length) {
    return (int)do_syscall6(SYS_FTRUNCATE,
                            (uint64_t)(int64_t)fd,
                            (uint64_t)length, 0, 0, 0);
}

/* ── Process sync ───────────────────────────────────────────────── */

void sync(void) {
    do_syscall(SYS_SYNCFS);
}
