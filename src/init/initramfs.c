/* initramfs.c — Initramfs switch_root init
 *
 * This is the initramfs init process (PID 1 inside the initramfs).
 * It handles:
 *   1. Parsing root= kernel cmdline (supports UUID=, LABEL=, /dev/hdaX)
 *   2. Mounting the real root filesystem
 *   3. Moving mount points and pivot_root to the real root
 *   4. Exec'ing /sbin/init on the real root
 *
 * Item S162: Initramfs switch_root
 *
 * This is a standalone userspace binary — all syscalls are inline.
 * It is linked into the initramfs as /init.
 */

/* ── Syscall numbers ──────────────────────────────────────────────────── */

#define SYS_READ       0
#define SYS_WRITE      1
#define SYS_OPEN       2
#define SYS_CLOSE      3
#define SYS_EXIT       4
#define SYS_FORK       211
#define SYS_EXECVE     234
#define SYS_MOUNT      165
#define SYS_UMOUNT     166
#define SYS_PIVOT_ROOT 217
#define SYS_CHDIR      214
#define SYS_SETHOSTNAME 268
#define SYS_DUP2       241
#define SYS_STAT       201
#define SYS_ACCESS     21
#define SYS_GETDENTS   2178  /* For directory iteration */

#define O_RDONLY       0
#define O_WRONLY       1
#define O_RDWR         2
#define O_CREAT        64
#define O_TRUNC        512

#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#define NULL           ((void *)0)
typedef unsigned long size_t;

/* ── Helper constants ─────────────────────────────────────────────────── */

#define MAX_PATH       256
#define MAX_CMDLINE    4096
#define MAX_ARGS       16

/* ── Inline syscall (x86-64) ─────────────────────────────────────────── */

static inline long syscall(long num, long a1, long a2, long a3,
                           long a4, long a5)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* ── Thin wrappers ───────────────────────────────────────────────────── */

static inline long write(int fd, const void *buf, unsigned long count)
{
    return syscall(SYS_WRITE, fd, (long)buf, count, 0, 0);
}

static inline long read(int fd, void *buf, unsigned long count)
{
    return syscall(SYS_READ, fd, (long)buf, count, 0, 0);
}

static inline int open(const char *path, int flags)
{
    return (int)syscall(SYS_OPEN, (long)path, flags, 0, 0, 0);
}

static inline int close(int fd)
{
    return (int)syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
}

static inline void exit(int code)
{
    syscall(SYS_EXIT, code, 0, 0, 0, 0);
    for (;;) __asm__ volatile("hlt");
}

static inline int mount(const char *src, const char *target,
                        const char *type, unsigned long flags,
                        const void *data)
{
    return (int)syscall(SYS_MOUNT, (long)src, (long)target,
                        (long)type, (long)flags, (long)data);
}

static inline int umount(const char *target)
{
    return (int)syscall(SYS_UMOUNT, (long)target, 0, 0, 0, 0);
}

static inline int pivot_root(const char *new_root, const char *put_old)
{
    return (int)syscall(SYS_PIVOT_ROOT, (long)new_root,
                        (long)put_old, 0, 0, 0);
}

static inline int chdir(const char *path)
{
    return (int)syscall(SYS_CHDIR, (long)path, 0, 0, 0, 0);
}

static inline int execve(const char *path, char *const argv[],
                         char *const envp[])
{
    return (int)syscall(SYS_EXECVE, (long)path, (long)argv,
                        (long)envp, 0, 0);
}

static inline int dup2(int oldfd, int newfd)
{
    return (int)syscall(SYS_DUP2, oldfd, newfd, 0, 0, 0);
}

/* ── String helpers ──────────────────────────────────────────────────── */

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0')
            return 0;
    }
    return 0;
}

static char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

static char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : 0;
}

/* ── Console output ──────────────────────────────────────────────────── */

static void puts(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

static void put_dec(int n)
{
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    if (n < 0) { puts("-"); n = -n; }
    if (n == 0) { puts("0"); return; }
    while (n > 0 && i > 0) {
        buf[--i] = '0' + (n % 10);
        n /= 10;
    }
    puts(buf + i);
}

/* ── Kernel command line parsing ─────────────────────────────────────── */

/*
 * Read /proc/cmdline or /dev/cmdline to get kernel parameters.
 * Returns 0 on success, -1 on failure.
 */
static int read_cmdline(char *buf, size_t maxlen)
{
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        fd = open("/dev/cmdline", O_RDONLY);
    if (fd < 0)
        return -1;

    long n = read(fd, buf, maxlen - 1);
    close(fd);

    if (n <= 0)
        return -1;

    /* Strip trailing newline */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
        n--;
    buf[n] = '\0';
    return 0;
}

/*
 * Find the value of a kernel parameter.
 * Returns pointer to the value part (after '='), or NULL if not found.
 */
static const char *find_cmdline_param(const char *cmdline, const char *param)
{
    size_t plen = strlen(param);
    const char *p = cmdline;

    while (*p) {
        /* Skip leading spaces */
        while (*p == ' ') p++;
        if (!*p) break;

        if (strncmp(p, param, plen) == 0 && p[plen] == '=') {
            return p + plen + 1;  /* point to value after '=' */
        }

        /* Skip to next space */
        while (*p && *p != ' ') p++;
    }
    return NULL;
}

/*
 * Extract a space-delimited token from a string.
 * Advances *pp to after the token.
 * Returns 1 if a token was found, 0 at end of string.
 */
static int next_token(const char **pp, char *buf, size_t maxlen)
{
    const char *p = *pp;

    /* Skip leading spaces */
    while (*p == ' ') p++;
    if (!*p) return 0;

    size_t i = 0;
    while (*p && *p != ' ' && i < maxlen - 1)
        buf[i++] = *p++;
    buf[i] = '\0';

    *pp = p;
    return 1;
}

/* ── Root device resolution ──────────────────────────────────────────── */

/*
 * Try to resolve a root= specification to a device path.
 * Supports:
 *   /dev/sdaX, /dev/hdaX, /dev/vdaX, /dev/nvmeXnYpZ — block device path
 *   UUID=... — resolve via /dev/disk/by-uuid/
 *   LABEL=... — resolve via /dev/disk/by-label/
 *
 * Returns 0 on success with resolved path in buf, -1 on failure.
 */
static int resolve_root_device(const char *root_spec, char *buf, size_t maxlen)
{
    if (!root_spec || !root_spec[0])
        return -1;

    /* Direct device path */
    if (root_spec[0] == '/') {
        strncpy(buf, root_spec, maxlen - 1);
        buf[maxlen - 1] = '\0';
        return 0;
    }

    /* UUID= format */
    if (strncmp(root_spec, "UUID=", 5) == 0) {
        const char *uuid = root_spec + 5;
        /* Build path: /dev/disk/by-uuid/<uuid> */
        char path[MAX_PATH];
        strncpy(path, "/dev/disk/by-uuid/", MAX_PATH - 1);
        size_t plen = strlen(path);
        strncpy(path + plen, uuid, MAX_PATH - plen - 1);
        path[MAX_PATH - 1] = '\0';

        /* Check if it exists (access syscall) */
        if (syscall(SYS_ACCESS, (long)path, 0, 0, 0, 0) == 0) {
            strncpy(buf, path, maxlen - 1);
            buf[maxlen - 1] = '\0';
            return 0;
        }

        /* Fallback: try matching via /sys/block iteration (simplified) */
        strncpy(buf, root_spec, maxlen - 1);
        buf[maxlen - 1] = '\0';
        puts("[initramfs] UUID= not resolved, trying as-is\n");
        return 0;
    }

    /* LABEL= format */
    if (strncmp(root_spec, "LABEL=", 6) == 0) {
        const char *label = root_spec + 6;
        char path[MAX_PATH];
        strncpy(path, "/dev/disk/by-label/", MAX_PATH - 1);
        size_t plen = strlen(path);
        strncpy(path + plen, label, MAX_PATH - plen - 1);
        path[MAX_PATH - 1] = '\0';

        if (syscall(SYS_ACCESS, (long)path, 0, 0, 0, 0) == 0) {
            strncpy(buf, path, maxlen - 1);
            buf[maxlen - 1] = '\0';
            return 0;
        }

        strncpy(buf, root_spec, maxlen - 1);
        buf[maxlen - 1] = '\0';
        puts("[initramfs] LABEL= not resolved, trying as-is\n");
        return 0;
    }

    /* Unknown format — use as-is */
    strncpy(buf, root_spec, maxlen - 1);
    buf[maxlen - 1] = '\0';
    return 0;
}

/* Simple strstr implementation for cmdline parsing */
static const char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
}

/* ── Main initramfs init ─────────────────────────────────────────────── */

void _start(void)
{
    char root_spec[MAX_PATH] = {0};
    char root_device[MAX_PATH] = {0};
    char cmdline[MAX_CMDLINE] = {0};
    int root_rw = 0;

    puts("[initramfs] Starting initramfs init...\n");

    /* Mount proc so we can read /proc/cmdline */
    puts("[initramfs] Mounting proc...\n");
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* Try /dev based mounting if /proc doesn't exist yet */
        puts("[initramfs] Mounting proc via devtmpfs...\n");
        mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
        mount("proc", "/proc", "proc", 0, NULL);
    }

    /* Mount sysfs */
    puts("[initramfs] Mounting sysfs...\n");
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    /* Read kernel command line */
    if (read_cmdline(cmdline, sizeof(cmdline)) != 0) {
        puts("[initramfs] WARNING: Cannot read cmdline, using defaults\n");
        strncpy(cmdline, "root=/dev/sda1 ro", sizeof(cmdline) - 1);
        cmdline[sizeof(cmdline) - 1] = '\0';
    }

    puts("[initramfs] Cmdline: ");
    puts(cmdline);
    puts("\n");

    /* Parse root= parameter */
    const char *root_val = find_cmdline_param(cmdline, "root");
    if (root_val) {
        strncpy(root_spec, root_val, MAX_PATH - 1);
        root_spec[MAX_PATH - 1] = '\0';
        /* Strip trailing space if present */
        char *sp = strchr(root_spec, ' ');
        if (sp) *sp = '\0';
    } else {
        puts("[initramfs] No root= parameter found\n");
        strncpy(root_spec, "/dev/sda1", MAX_PATH - 1);
    }

    /* Check for rw flag */
    if (strstr(cmdline, " rw") || strncmp(cmdline, "rw", 2) == 0)
        root_rw = 1;

    puts("[initramfs] Root specification: ");
    puts(root_spec);
    puts("\n");

    /* Resolve root device */
    if (resolve_root_device(root_spec, root_device, sizeof(root_device)) != 0) {
        puts("[initramfs] ERROR: Cannot resolve root device\n");
        goto emergency_shell;
    }

    puts("[initramfs] Resolved root device: ");
    puts(root_device);
    puts("\n");

    /* Create mount point for real root */
    puts("[initramfs] Creating /mnt/root...\n");
    syscall(SYS_ACCESS, (long)"/mnt/root", 0, 0, 0, 0);
    /* mkdir equivalent via open(O_CREAT) on parent */
    int fd = open("/mnt/root", O_RDONLY);
    if (fd < 0) {
        /* We need to mkdir. Since we don't have a syscall for mkdir,
         * we try the rootfs's mkdir via the kernel's VFS. For initramfs
         * this should already exist. */
        puts("[initramfs] WARNING: /mnt/root not accessible\n");
    } else {
        close(fd);
    }

    /* Mount the real root filesystem */
    puts("[initramfs] Mounting real root...\n");
    {
        unsigned long mflags = root_rw ? 0 : 1; /* MS_RDONLY = 1 */
        if (mount(root_device, "/mnt/root", NULL, mflags, NULL) != 0) {
            puts("[initramfs] Mount failed, trying without fs type...\n");
            if (mount(root_device, "/mnt/root", "ext2", mflags, NULL) != 0) {
                puts("[initramfs] ERROR: Cannot mount root filesystem!\n");
                goto emergency_shell;
            }
        }
    }

    puts("[initramfs] Root filesystem mounted successfully\n");

    /* Move essential mounts into the real root */
    puts("[initramfs] Moving mounts to real root...\n");
    mount("/dev", "/mnt/root/dev", NULL, 2, NULL);  /* MS_MOVE = 2 */
    mount("/proc", "/mnt/root/proc", NULL, 2, NULL);
    mount("/sys", "/mnt/root/sys", NULL, 2, NULL);

    /* Change directory to the new root */
    if (chdir("/mnt/root") != 0) {
        puts("[initramfs] ERROR: Cannot chdir to /mnt/root\n");
        goto emergency_shell;
    }

    /* Try pivot_root */
    puts("[initramfs] Performing pivot_root...\n");
    if (pivot_root(".", ".") == 0) {
        /* Unmount the old root (our initramfs) */
        umount("/");
    } else {
        puts("[initramfs] pivot_root failed, trying switch_root approach...\n");
        /* Fallback: just exec chroot-like via the real init */
        /* We can't actually chroot without the chroot syscall,
         * so we exec /sbin/init from the mounted root. */
    }

    /* Execute the real init on the real root */
    puts("[initramfs] Exec'ing /sbin/init on real root...\n");

    char *argv[] = { "/sbin/init", NULL };
    char *envp[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };

    execve("/mnt/root/sbin/init", argv, envp);
    execve("/sbin/init", argv, envp);
    execve("/mnt/root/bin/sh", argv, NULL);

    /* Everything failed — drop to emergency shell */
    puts("[initramfs] ERROR: Cannot execute real init!\n");

emergency_shell:
    puts("[initramfs] Dropping to emergency shell...\n");
    puts("[initramfs] Type 'reboot' to reboot, or 'exit' to try again\n");

    char *sh_argv[] = { "/bin/sh", NULL };
    char *sh_envp[] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };
    execve("/bin/sh", sh_argv, sh_envp);
    execve("/bin/ash", sh_argv, sh_envp);

    /* Last resort: infinite loop */
    puts("[initramfs] Cannot start shell — halting\n");
    for (;;)
        __asm__ volatile("hlt");
}

/* Simple strstr implementation for cmdline parsing */
static const char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
}
