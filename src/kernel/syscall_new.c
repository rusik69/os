/* syscall_new.c — Self-contained syscall implementations
 *
 * Provides: dup, dup2, dup3, pipe2, sysinfo, getrandom,
 *           prctl, utimensat, futimens, fadvise, readlinkat, symlinkat
 *
 * These functions do NOT depend on static helpers from syscall.c.
 */

#include "syscall.h"
#include "process.h"
#include "vfs.h"
#include "fs.h"
#include "sched_attr.h"
#include "scheduler.h"
#include "timer.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "uaccess.h"
#include "pipe.h"
#include "seccomp_bpf.h"

/* ── Simple PRNG (xorshift64) ─────────────────────────────────────── */
static uint64_t rng_state = 0;

static uint64_t local_xorshift64(void)
{
    if (rng_state == 0)
        rng_state = timer_get_ticks() + 0xDEADBEEFCAFEBABEULL;
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/* ── Helper: find free fd slot ────────────────────────────────────── */
static int find_free_fd(struct process *proc)
{
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!proc->fd_table[i].used)
            return i;
    }
    return -1;
}

/* ── Helper: resolve a path relative to dirfd or cwd ──────────────── */
static const char *resolve_at(int dirfd, const char *path, char *buf, size_t bufsz)
{
    if (!path) return NULL;

    if (path[0] == '/') {
        strncpy(buf, path, bufsz - 1);
        buf[bufsz - 1] = '\0';
        return buf;
    }

    struct process *proc = process_get_current();
    if (!proc) return NULL;

    /* Relative path: prepend dirfd's path or cwd */
    if (dirfd == -100) { /* AT_FDCWD */
        strncpy(buf, proc->cwd, bufsz - 1);
        buf[bufsz - 1] = '\0';
    } else if (dirfd >= 0 && dirfd < PROCESS_FD_MAX && proc->fd_table[dirfd].used) {
        strncpy(buf, proc->fd_table[dirfd].path, bufsz - 1);
        buf[bufsz - 1] = '\0';
    } else {
        return NULL;
    }

    size_t cur_len = strlen(buf);
    if (cur_len + 1 + strlen(path) >= bufsz)
        return NULL;

    if (buf[cur_len] != '/' && buf[cur_len] != '\0') {
        strncat(buf, "/", bufsz - strlen(buf) - 1);
    }
    strncat(buf, path, bufsz - strlen(buf) - 1);
    return buf;
}

/* ====================================================================
 * dup / dup2 / dup3
 * ==================================================================== */

static int do_dup(int old_fd)
{
    struct process *proc = process_get_current();
    if (!proc) return -1;
    if (old_fd >= PROCESS_FD_MAX || !proc->fd_table[old_fd].used)
        return -EBADF;

    int new_fd = find_free_fd(proc);
    if (new_fd < 0) return -EMFILE;

    proc->fd_table[new_fd] = proc->fd_table[old_fd];
    proc->fd_table[new_fd].offset = proc->fd_table[old_fd].offset;
    return new_fd;
}

static int do_dup2(int old_fd, int new_fd)
{
    struct process *proc = process_get_current();
    if (!proc) return -1;
    if (old_fd >= PROCESS_FD_MAX || !proc->fd_table[old_fd].used)
        return -EBADF;
    if (new_fd >= PROCESS_FD_MAX) return -EBADF;

    if (old_fd == new_fd) return new_fd;

    if (proc->fd_table[new_fd].used)
        memset(&proc->fd_table[new_fd], 0, sizeof(struct process_fd));

    proc->fd_table[new_fd] = proc->fd_table[old_fd];
    proc->fd_table[new_fd].offset = proc->fd_table[old_fd].offset;
    return new_fd;
}

static int do_dup3(int old_fd, int new_fd, int flags)
{
    if (flags & ~O_CLOEXEC)
        return -EINVAL;

    struct process *proc = process_get_current();
    if (!proc) return -1;
    if (old_fd >= PROCESS_FD_MAX || !proc->fd_table[old_fd].used)
        return -EBADF;
    if (new_fd >= PROCESS_FD_MAX)
        return -EBADF;

    if (old_fd == new_fd) {
        if (flags & O_CLOEXEC)
            proc->fd_table[new_fd].flags |= FD_CLOEXEC;
        else
            proc->fd_table[new_fd].flags = (uint8_t)(proc->fd_table[new_fd].flags & ~(unsigned)FD_CLOEXEC);
        return new_fd;
    }

    if (proc->fd_table[new_fd].used)
        memset(&proc->fd_table[new_fd], 0, sizeof(struct process_fd));

    proc->fd_table[new_fd] = proc->fd_table[old_fd];

    if (flags & O_CLOEXEC)
        proc->fd_table[new_fd].flags |= FD_CLOEXEC;
    else
        proc->fd_table[new_fd].flags = (uint8_t)(proc->fd_table[new_fd].flags & ~(unsigned)FD_CLOEXEC);

    return new_fd;
}

/* ====================================================================
 * pipe2
 * ==================================================================== */

static int do_pipe2(int fds[2], int flags)
{
    if (flags & ~(O_CLOEXEC | 04000)) /* O_NONBLOCK = 04000 */
        return -EINVAL;

    if (!fds) return -EFAULT;

    struct process *proc = process_get_current();
    if (!proc) return -1;

    int id = pipe_create();
    if (id < 0) return -1;

    if ((flags & 04000))
        pipe_set_nonblock(id, 1);

    int read_fd = -1, write_fd = -1;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!proc->fd_table[i].used) {
            if (read_fd < 0) read_fd = i;
            else if (write_fd < 0) { write_fd = i; break; }
        }
    }
    if (read_fd < 0 || write_fd < 0) return -EMFILE;

    proc->fd_table[read_fd].used = 1;
    proc->fd_table[read_fd].offset = (uint32_t)id;
    snprintf(proc->fd_table[read_fd].path, 64, "pipe_read_%d", id);
    proc->fd_table[read_fd].flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;

    proc->fd_table[write_fd].used = 1;
    proc->fd_table[write_fd].offset = (uint32_t)id;
    snprintf(proc->fd_table[write_fd].path, 64, "pipe_write_%d", id);
    proc->fd_table[write_fd].flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;

    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

/* ====================================================================
 * sysinfo
 * ==================================================================== */

static int do_sysinfo(struct sysinfo *info)
{
    if (!info) return -EFAULT;
    memset(info, 0, sizeof(*info));

    info->uptime = timer_get_ticks() / TIMER_FREQ;
    info->totalram = (uint64_t)pmm_get_total_frames() * 4096ULL;
    info->freeram = (uint64_t)(pmm_get_total_frames() - pmm_get_used_frames()) * 4096ULL;
    info->procs = 0;
    info->mem_unit = 1;

    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED)
            info->procs++;
    }

    /* Load averages: count running/ready processes as a simple load metric */
    int run = 0;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_RUNNING || table[i].state == PROCESS_READY)
            run++;
    }
    /* Scale by 1024 (SI_LOAD_SHIFT convention) */
    info->loads[0] = (uint64_t)run * 1024ULL;
    info->loads[1] = (uint64_t)run * 1024ULL;
    info->loads[2] = (uint64_t)run * 1024ULL;

    return 0;
}

/* ====================================================================
 * getrandom
 * ==================================================================== */

static ssize_t do_getrandom(void *buf, size_t count, unsigned int flags)
{
    (void)flags;
    if (!buf || count == 0) return 0;
    if (count > 4096) count = 4096;

    uint8_t *b = (uint8_t *)buf;
    for (size_t i = 0; i < count; i++)
        b[i] = (uint8_t)(local_xorshift64() >> 56);

    return (ssize_t)count;
}

/* ====================================================================
 * prctl
 * ==================================================================== */

/* ── prctl operations ──────────────────────────────────────────────── */
#ifndef PR_SET_NAME
#define PR_SET_NAME            15
#endif
#ifndef PR_GET_NAME
#define PR_GET_NAME            16
#endif
#ifndef PR_SET_PDEATHSIG
#define PR_SET_PDEATHSIG       1
#endif
#ifndef PR_GET_PDEATHSIG
#define PR_GET_PDEATHSIG       2
#endif
#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS    38
#endif
#ifndef PR_GET_NO_NEW_PRIVS
#define PR_GET_NO_NEW_PRIVS    39
#endif
#ifndef PR_SET_SECCOMP
#define PR_SET_SECCOMP         22
#endif
#ifndef PR_GET_SECCOMP
#define PR_GET_SECCOMP         21
#endif
#ifndef PR_SET_SECUREBITS
#define PR_SET_SECUREBITS      28
#endif
#ifndef PR_GET_SECUREBITS
#define PR_GET_SECUREBITS      27
#endif
#ifndef PR_SET_DUMPABLE
#define PR_SET_DUMPABLE        4
#endif
#ifndef PR_GET_DUMPABLE
#define PR_GET_DUMPABLE        3
#endif

#ifndef SECBIT_KEEP_CAPS
#define SECBIT_KEEP_CAPS           1
#endif
#ifndef SECBIT_KEEP_CAPS_LOCKED
#define SECBIT_KEEP_CAPS_LOCKED    2
#endif
#ifndef SECBIT_NO_SETUID_FIXUP
#define SECBIT_NO_SETUID_FIXUP     4
#endif
#ifndef SECBIT_NO_SETUID_FIXUP_LOCKED
#define SECBIT_NO_SETUID_FIXUP_LOCKED 8
#endif
#ifndef SECBIT_NOROOT
#define SECBIT_NOROOT             16
#endif
#ifndef SECBIT_NOROOT_LOCKED
#define SECBIT_NOROOT_LOCKED      32
#endif
#ifndef SECBIT_ALLOWED_MASK
#define SECBIT_ALLOWED_MASK       (SECBIT_KEEP_CAPS | SECBIT_NO_SETUID_FIXUP | SECBIT_NOROOT)
#endif
#ifndef SECBIT_LOCKED_MASK
#define SECBIT_LOCKED_MASK        (SECBIT_KEEP_CAPS_LOCKED | SECBIT_NO_SETUID_FIXUP_LOCKED | SECBIT_NOROOT_LOCKED)
#endif

static int do_prctl(int option, unsigned long arg2, unsigned long arg3,
             unsigned long arg4, unsigned long arg5)
{
    (void)arg3; (void)arg4; (void)arg5;
    struct process *p = process_get_current();
    if (!p) return -1;

    switch (option) {
    case PR_SET_NAME:
        if (!arg2) return -EFAULT;
        memset(p->proc_comm, 0, 16);
        memcpy(p->proc_comm, (const char *)arg2, 15);
        p->proc_comm[15] = '\0';
        return 0;

    case PR_GET_NAME: {
        char name[16];
        memcpy(name, p->proc_comm, 16);
        if (copy_to_user(arg2, name, 16) < 0)
            return -EFAULT;
        return 0;
    }

    case PR_SET_NO_NEW_PRIVS:
        if (arg2 != 1 || p->no_new_privs) return -EPERM;
        p->no_new_privs = 1;
        return 0;

    case PR_GET_NO_NEW_PRIVS:
        return p->no_new_privs;

    case PR_SET_DUMPABLE: {
        int val = (int)arg2;
        if (val < 0 || val > 2) return -EINVAL;
        p->dumpable = val;
        return 0;
    }

    case PR_GET_DUMPABLE:
        return p->dumpable;

    case PR_SET_PDEATHSIG:
    case PR_GET_PDEATHSIG:
        /* Not fully implemented */
        return 0;

    case PR_SET_SECCOMP: {
        /* Forward to seccomp — install a BPF filter */
        if (arg2 != SECCOMP_MODE_FILTER_BPF) {
            /* Only SECCOMP_SET_MODE_FILTER (mode=2) is supported */
            return -EINVAL;
        }
        struct sock_fprog prog;
        if (copy_from_user(&prog, arg3, sizeof(prog)) < 0)
            return -EFAULT;
        int ret = seccomp_filter_install(&prog);
        if (ret == 0)
            p->no_new_privs = 1;  /* PR_SET_SECCOMP requires no_new_privs */
        return ret;
    }

    case PR_GET_SECCOMP:
        return 0;

    case PR_SET_SECUREBITS:
    case PR_GET_SECUREBITS:
        /* Not fully implemented in standalone mode */
        return 0;

    default:
        return -EINVAL;
    }
}

/* ====================================================================
 * utimensat / futimens
 * ==================================================================== */

#ifndef UTIME_NOW
#define UTIME_NOW   ((1L << 30) - 1)
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT  ((1L << 30) - 2)
#endif

static int do_utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags)
{
    (void)flags;
    char path[256];

    if (!pathname) {
        if (dirfd == -100) return -EINVAL;
        struct process *p = process_get_current();
        if (!p || dirfd >= PROCESS_FD_MAX || !p->fd_table[dirfd].used)
            return -EBADF;
        strncpy(path, p->fd_table[dirfd].path, 255);
        path[255] = '\0';
    } else {
        char kpath[256];
        if (copy_from_user(kpath, (uint64_t)(uintptr_t)pathname, sizeof(kpath)) < 0)
            return -EFAULT;
        const char *resolved = resolve_at(dirfd, kpath, path, sizeof(path));
        if (!resolved) return -ENOENT;
        memmove(path, resolved, 256);
    }

    uint32_t now_sec = (uint32_t)(timer_get_ticks() / TIMER_FREQ);
    uint32_t new_mtime = now_sec;

    if (times) {
        struct timespec ts[2];
        if (copy_from_user(ts, (uint64_t)(uintptr_t)times, sizeof(ts)) < 0)
            return -EFAULT;

        if (ts[1].tv_nsec == UTIME_OMIT) {
            struct vfs_stat st;
            if (vfs_stat(path, &st) < 0) return -1;
            new_mtime = st.mtime;
        } else if (ts[1].tv_nsec == UTIME_NOW) {
            new_mtime = now_sec;
        } else {
            new_mtime = (uint32_t)ts[1].tv_sec;
        }
    }

    return fs_set_mtime(path, new_mtime);
}

static int do_futimens(int fd, const struct timespec times[2])
{
    struct process *p = process_get_current();
    if (!p || fd >= PROCESS_FD_MAX || !p->fd_table[fd].used)
        return -EBADF;

    const char *path = p->fd_table[fd].path;
    if (!path || !path[0]) return -ENOENT;

    uint32_t now_sec = (uint32_t)(timer_get_ticks() / TIMER_FREQ);
    uint32_t new_mtime = now_sec;

    if (times) {
        struct timespec ts[2];
        if (copy_from_user(ts, (uint64_t)(uintptr_t)times, sizeof(ts)) < 0)
            return -EFAULT;

        if (ts[1].tv_nsec == UTIME_OMIT) {
            struct vfs_stat st;
            if (vfs_stat(path, &st) < 0) return -1;
            new_mtime = st.mtime;
        } else if (ts[1].tv_nsec == UTIME_NOW) {
            new_mtime = now_sec;
        } else {
            new_mtime = (uint32_t)ts[1].tv_sec;
        }
    }

    return fs_set_mtime(path, new_mtime);
}

/* ====================================================================
 * posix_fadvise
 * ==================================================================== */

#ifndef POSIX_FADV_NORMAL
#define POSIX_FADV_NORMAL       0
#define POSIX_FADV_RANDOM       1
#define POSIX_FADV_SEQUENTIAL   2
#define POSIX_FADV_WILLNEED     3
#define POSIX_FADV_DONTNEED     4
#define POSIX_FADV_NOREUSE      5
#endif

static int do_fadvise64(int fd, uint64_t offset, uint64_t len, int advice)
{
    if (advice > POSIX_FADV_NOREUSE)
        return -EINVAL;

    struct process *proc = process_get_current();
    if (!proc || fd >= PROCESS_FD_MAX || !proc->fd_table[fd].used)
        return -EBADF;

    proc->fd_table[fd].advice = advice;
    const char *path = proc->fd_table[fd].path;

    if (advice == POSIX_FADV_WILLNEED && path[0]) {
        vfs_readahead(path, (uint32_t)offset,
                     (uint32_t)(len ? len : 4096));
    }

    return 0;
}

/* ====================================================================
 * readlinkat / symlinkat
 * ==================================================================== */

static ssize_t do_readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz)
{
    char kpath[256];
    char resolved[256];

    if (copy_from_user(kpath, (uint64_t)(uintptr_t)pathname, sizeof(kpath)) < 0)
        return -EFAULT;

    const char *path = resolve_at(dirfd, kpath, resolved, sizeof(resolved));
    if (!path) return -ENOENT;

    char link_target[256];
    int ret = fs_readlink(path, link_target, sizeof(link_target));
    if (ret < 0) return ret;

    size_t copy_len = (bufsiz < (size_t)ret) ? bufsiz : (size_t)ret;
    if (copy_to_user((uint64_t)(uintptr_t)buf, link_target, copy_len) < 0)
        return -EFAULT;

    return (ssize_t)copy_len;
}

static int do_symlinkat(const char *target, int newdirfd, const char *linkpath)
{
    char ktarget[256];
    char klinkpath[256];
    char resolved[256];

    if (copy_from_user(ktarget, (uint64_t)(uintptr_t)target, sizeof(ktarget)) < 0)
        return -EFAULT;
    if (copy_from_user(klinkpath, (uint64_t)(uintptr_t)linkpath, sizeof(klinkpath)) < 0)
        return -EFAULT;

    const char *path = resolve_at(newdirfd, klinkpath, resolved, sizeof(resolved));
    if (!path) return -ENOENT;

    return fs_symlink(ktarget, path);
}

/* ── syscall_new_register ──────────────────────────────────── */
static int syscall_new_register(int nr, void *handler)
{
    if (nr < 0 || nr > 255 || !handler)
        return -EINVAL;

    /* Use a static table local to this file so that syscall_new.c
     * remains independent of the main syscall dispatcher. */
    static void *new_syscall_table[256];
    static int table_initialized = 0;

    if (!table_initialized) {
        memset(new_syscall_table, 0, sizeof(new_syscall_table));
        table_initialized = 1;
    }

    if (new_syscall_table[nr] != NULL) {
        kprintf("[syscall] syscall_new_register: nr=%d already registered\n", nr);
        return -EBUSY;
    }

    new_syscall_table[nr] = handler;
    kprintf("[syscall] syscall_new_register: nr=%d handler=0x%llx\n",
            nr, (unsigned long long)(uintptr_t)handler);
    return 0;
}
/* ── syscall_new_unregister ────────────────────────────────── */
static int syscall_new_unregister(int nr)
{
    if (nr < 0 || nr > 255)
        return -EINVAL;

    static void *new_syscall_table[256];
    static int table_initialized = 0;

    if (!table_initialized) return -EINVAL;
    if (!new_syscall_table[nr]) return -ENOENT;

    new_syscall_table[nr] = NULL;
    kprintf("[syscall] syscall_new_unregister: nr=%d\n", nr);
    return 0;
}
/* ── syscall_new_invoke ────────────────────────────────────── */
static int syscall_new_invoke(int nr, void *args)
{
    if (nr < 0 || nr > 255)
        return -EINVAL;

    static void *new_syscall_table[256];
    static int table_initialized = 0;

    if (!table_initialized) return 0;

    void *handler = new_syscall_table[nr];
    if (!handler) {
        kprintf("[syscall] syscall_new_invoke: nr=%d not registered\n", nr);
        return 0;
    }

    /* A registered handler is a function pointer matching:
     *   int handler(void *args)
     * Cast and call it. */
    typedef int (*syscall_handler_t)(void *);
    union { void *obj; syscall_handler_t func; } conv = { .obj = handler };
    return conv.func(args);
}
