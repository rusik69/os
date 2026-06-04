/*
 * init.c — PID 1: Service supervision, orphan reaping, inittab parsing
 *
 * This is the first userspace process.  Responsibilities:
 *   1. Parse /etc/inittab for boot-time service configuration
 *   2. Fork+exec each configured service
 *   3. Reap orphaned children (SIGCHLD handling via waitpid loop)
 *   4. Restart critical services when they crash
 *   5. Handle shutdown/reboot signals
 *
 * Inittab format (traditional SysV):
 *   id:runlevels:action:process
 *
 * Actions: respawn, once, wait, sysinit, boot, bootwait, askfirst, off
 *
 * This is a standalone userspace binary (no libc) — all syscalls
 * are inlined, and basic string/utility functions are self-contained.
 */

/* ── Syscall numbers ──────────────────────────────────────────────────── */

#define SYS_READ      0
#define SYS_WRITE     1
#define SYS_OPEN      2
#define SYS_CLOSE     3
#define SYS_EXIT      4
#define SYS_FORK      211
#define SYS_SIGNAL    213
#define SYS_EXECVE    234
#define SYS_WAITPID   119
#define SYS_GETPID    212
#define SYS_KILL      214
#define SYS_REBOOT    88
#define SYS_SETHOSTNAME 268

#define SIGCHLD       17
#define SIGTERM       15
#define SIGINT        2
#define SIGHUP        1
#define SIGIGN        1L
#define SIGDFL        0L

#define O_RDONLY      0
#define O_WRONLY      1
#define O_CREAT       64
#define O_TRUNC       512

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define WNOHANG       1
#define WUNTRACED     2

#define MAX_SERVICES  32
#define MAX_LINE      256
#define MAX_ARGS      16
#define MAX_PATH      128

/* Runlevel constants (Item U4) */
#define DEFAULT_RUNLEVEL    2   /* multi-user default */
#define INITPIPE_PATH       "/var/run/initpipe"

/* Avoid requiring <stddef.h> */
#define NULL          ((void *)0)
typedef unsigned long size_t;

/* ── Service state ────────────────────────────────────────────────────── */

enum service_action {
    ACT_RESPAWN,    /* restart when dies */
    ACT_ONCE,       /* run once, don't restart */
    ACT_WAIT,       /* run and wait for completion before continuing */
    ACT_SYSINIT,    /* run during sysinit */
    ACT_BOOT,       /* run at boot */
    ACT_BOOTWAIT,   /* run at boot, wait for completion */
    ACT_ASKFIRST,   /* ask user before starting (console only) */
    ACT_OFF,        /* disabled */
};

enum service_state {
    ST_DEAD,
    ST_RUNNING,
    ST_WAITING,     /* wait action in progress */
};

struct service {
    char            id[16];
    enum service_action action;
    enum service_state  state;
    char            path[MAX_PATH];
    char           *argv[MAX_ARGS + 1];   /* NULL-terminated */
    int             argc;
    int             pid;                   /* 0 = not running */
    int             critical;              /* respawn limit applies */
    int             respawn_count;
    int             respawn_limit;         /* max respawns before giving up */
    unsigned int    runlevels;             /* bitmask of allowed runlevels (Item U4) */
};

static struct service services[MAX_SERVICES];
static int service_count = 0;
static volatile int shutdown_requested = 0;

/* Current system runlevel (0=halt, 1=single, 2=multi-user, 5=graphical) */
static int current_runlevel = DEFAULT_RUNLEVEL;

/* ── Forward declarations ─────────────────────────────────────────────── */

static void init_parse_inittab(void);
static void service_start(struct service *svc);
static void service_stop(struct service *svc);
static void reap_children(void);
static void boot_services(void);
static unsigned int runlevel_bitmask(int rl);
static void set_runlevel(int new_rl);
static void check_initpipe(void);

/* Console output forward declarations (used by runlevel functions before they are defined) */
static void puts(const char *s);
static void put_dec(int n);

/* Read /etc/hostname at boot and set kernel hostname (Item U23) */
static void init_set_hostname(void);

/* ── Inline syscall (x86-64: args in RDI, RSI, RDX, R10, R8, R9) ──── */

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

static inline int fork(void)
{
    return (int)syscall(SYS_FORK, 0, 0, 0, 0, 0);
}

static inline int getpid(void)
{
    return (int)syscall(SYS_GETPID, 0, 0, 0, 0, 0);
}

static inline int kill(int pid, int sig)
{
    return (int)syscall(SYS_KILL, pid, sig, 0, 0, 0);
}

static inline int waitpid(int pid, int *status, int options)
{
    return (int)syscall(SYS_WAITPID, pid, (long)status, options, 0, 0);
}

static inline void signal(int signum, long handler)
{
    syscall(SYS_SIGNAL, signum, handler, 0, 0, 0);
}

static inline int sethostname(const char *name, size_t len)
{
    if (!name || len == 0) return -1;
    return (int)syscall(SYS_SETHOSTNAME, (long)name, (long)len, 0, 0, 0);
}

/* ── String helpers (avoid libc dependency) ───────────────────────────── */

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
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

static int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : 0;
}

/* ── Runlevel helper ─────────────────────────────────────────────────── */

/* Convert a runlevel digit (0-9) to a bitmask for matching against
 * the service's runlevels field.  Returns 0 for invalid runlevels. */
static unsigned int runlevel_bitmask(int rl)
{
    if (rl < 0 || rl > 9) return 0;
    return 1u << rl;
}

/* ── Inittab parsing ──────────────────────────────────────────────────── */

/*
 * Parse one line of /etc/inittab.
 * Format:  id:runlevels:action:process
 *   id       ::= 1-4 char identifier
 *   runlevels ::= string of digits (e.g. "2345") indicating allowed runlevels
 *   action   ::= respawn | once | wait | sysinit | boot | bootwait | askfirst | off
 *   process  ::= full path to executable + optional arguments
 *
 * Runlevel semantics (SysV convention):
 *   0 = halt, 1 = single-user, 2 = multi-user (default),
 *   3 = networking, 4 = reserved, 5 = graphical, 6 = reboot, s/S = single
 */
static int parse_inittab_line(const char *line, struct service *svc)
{
    char buf[MAX_LINE];
    char *p;
    int field = 0;

    /* Strip trailing whitespace/newline */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                       line[len-1] == ' ' || line[len-1] == '\t'))
        len--;

    if (len == 0 || len >= MAX_LINE)
        return -1;

    /* Copy and parse fields by colon delimiter */
    for (size_t i = 0; i < len; i++)
        buf[i] = line[i];
    buf[len] = '\0';

    p = buf;

    while (p) {
        char *colon = strchr(p, ':');
        if (colon) *colon = '\0';

        switch (field) {
        case 0: /* id */
            strncpy(svc->id, p, sizeof(svc->id) - 1);
            svc->id[sizeof(svc->id) - 1] = '\0';
            break;
        case 1: /* runlevels — parse digit string into bitmask */
        {
            const char *rp = p;
            svc->runlevels = 0;
            while (rp && *rp) {
                if (*rp >= '0' && *rp <= '9')
                    svc->runlevels |= runlevel_bitmask(*rp - '0');
                else if (*rp == 's' || *rp == 'S')
                    svc->runlevels |= runlevel_bitmask(1);  /* 's' = single-user */
                rp++;
            }
            break;
        }
        case 2: /* action */
            if (strcmp(p, "respawn") == 0)      svc->action = ACT_RESPAWN;
            else if (strcmp(p, "once") == 0)     svc->action = ACT_ONCE;
            else if (strcmp(p, "wait") == 0)     svc->action = ACT_WAIT;
            else if (strcmp(p, "sysinit") == 0)  svc->action = ACT_SYSINIT;
            else if (strcmp(p, "boot") == 0)     svc->action = ACT_BOOT;
            else if (strcmp(p, "bootwait") == 0) svc->action = ACT_BOOTWAIT;
            else if (strcmp(p, "askfirst") == 0) svc->action = ACT_ASKFIRST;
            else if (strcmp(p, "off") == 0)      svc->action = ACT_OFF;
            else return -1;  /* unknown action */
            break;
        case 3: { /* process + args */
            char *args[MAX_ARGS];
            int argc = 0;

            /* First token = path */
            char *tok = p;
            char *space = strchr(tok, ' ');
            if (space) {
                *space = '\0';
                args[argc++] = tok;
                /* Parse remaining args */
                char *next = space + 1;
                while (next && *next && argc < MAX_ARGS - 1) {
                    /* Skip leading spaces */
                    while (*next == ' ') next++;
                    if (!*next) break;
                    char *sp = strchr(next, ' ');
                    if (sp) {
                        *sp = '\0';
                        args[argc++] = next;
                        next = sp + 1;
                    } else {
                        args[argc++] = next;
                        break;
                    }
                }
            } else {
                args[argc++] = tok;
            }
            args[argc] = NULL;

            strncpy(svc->path, args[0], sizeof(svc->path) - 1);
            svc->path[sizeof(svc->path) - 1] = '\0';
            svc->argc = argc;
            for (int i = 0; i <= argc; i++)
                svc->argv[i] = args[i];
            break;
        }
        default:
            break;
        }

        if (!colon) break;
        p = colon + 1;
        field++;
    }

    /* Must have at least id, action, and process */
    if (field < 3)
        return -1;

    svc->pid = 0;
    svc->state = ST_DEAD;
    svc->critical = (svc->action == ACT_RESPAWN);
    svc->respawn_count = 0;
    svc->respawn_limit = (svc->critical) ? 10 : 0;  /* max 10 respawns for critical */

    /* If no runlevels were explicitly specified, default to all runlevels */
    if (svc->runlevels == 0)
        svc->runlevels = ~0u;

    return 0;
}

static void init_parse_inittab(void)
{
    int fd = open("/etc/inittab", O_RDONLY);
    if (fd < 0) {
        /* No inittab — that's ok, we'll just reap */
        return;
    }

    char buf[512];
    long n;
    size_t pos = 0;

    while ((n = read(fd, buf + pos, sizeof(buf) - pos - 1)) > 0) {
        pos += (size_t)n;
        buf[pos] = '\0';

        char *line_start = buf;
        for (size_t i = 0; i < pos; i++) {
            if (buf[i] == '\n') {
                buf[i] = '\0';

                /* Skip comments and empty lines */
                if (line_start[0] && line_start[0] != '#') {
                    if (service_count < MAX_SERVICES) {
                        if (parse_inittab_line(line_start, &services[service_count]) == 0)
                            service_count++;
                    }
                }

                line_start = buf + i + 1;
            }
        }

        /* Move remaining partial line to start */
        size_t remaining = pos - (size_t)(line_start - buf);
        if (remaining > 0 && line_start != buf) {
            for (size_t i = 0; i < remaining; i++)
                buf[i] = line_start[i];
            pos = remaining;
        } else {
            pos = 0;
        }
    }

    close(fd);
}

/* ── Service lifecycle ────────────────────────────────────────────────── */

static void service_start(struct service *svc)
{
    if (svc->state == ST_RUNNING)
        return;

    int pid = fork();
    if (pid < 0) {
        /* Fork failed — try again later */
        return;
    }

    if (pid == 0) {
        /* Child: exec the service */
        /* Set up stdin/stdout/stderr */
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            /* We'd need dup2 but we don't have it. Just close and hope for the best. */
            close(fd);
        }

        /* Build argv array on stack for exec */
        char *argv[MAX_ARGS + 1];
        int argc = svc->argc;
        for (int i = 0; i < argc; i++)
            argv[i] = svc->argv[i];
        argv[argc] = NULL;

        /* Exec the service path with arguments and empty environment */
        char *empty_env[] = { NULL };
        syscall(SYS_EXECVE, (long)svc->path, (long)argv, (long)empty_env, 0, 0);

        /* If exec fails, exit */
        exit(1);
    }

    /* Parent: record state */
    svc->pid = pid;
    svc->state = ST_RUNNING;
}

static void service_stop(struct service *svc)
{
    if (svc->pid > 0) {
        kill(svc->pid, SIGTERM);
        /* Don't wait here — let reap_children handle it */
    }
    svc->state = ST_DEAD;
    svc->pid = 0;
}

/*
 * Handle a terminated child process.
 * Returns 1 if the service should be respawned, 0 otherwise.
 */
static int handle_child_exit(struct service *svc, int status)
{
    (void)status;

    if (svc->pid <= 0)
        return 0;

    svc->pid = 0;
    svc->state = ST_DEAD;

    switch (svc->action) {
    case ACT_RESPAWN:
        /* Only respawn if the service is allowed in the current runlevel */
        if (!(svc->runlevels & runlevel_bitmask(current_runlevel)))
            return 0;
        svc->respawn_count++;
        if (svc->respawn_limit > 0 && svc->respawn_count > svc->respawn_limit) {
            /* Exceeded respawn limit — give up */
            return 0;
        }
        /* Brief delay before respawn to avoid tight fork loops */
        for (volatile int i = 0; i < 1000000; i++)
            __asm__ volatile("pause");
        return 1;

    case ACT_ONCE:
    case ACT_SYSINIT:
    case ACT_BOOT:
    default:
        return 0;
    }
}

/*
 * Reap children: call waitpid in a loop until no more children have exited.
 * Then respawn any that need respawning.
 */
static void reap_children(void)
{
    int status;
    int pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Find the service that matches this PID */
        int found = 0;
        for (int i = 0; i < service_count; i++) {
            if (services[i].pid == pid) {
                found = 1;
                if (handle_child_exit(&services[i], status)) {
                    service_start(&services[i]);
                }
                break;
            }
        }
        /* If not a tracked service, it was an orphan — just reaped */
        (void)found;
    }
}

/* ── Boot sequence ────────────────────────────────────────────────────── */

static void boot_services(void)
{
    unsigned int rlmask = runlevel_bitmask(current_runlevel);

    /* Phase 1: sysinit — run and wait for each */
    for (int i = 0; i < service_count; i++) {
        if (!(services[i].runlevels & rlmask))
            continue;
        if (services[i].action == ACT_SYSINIT && services[i].state == ST_DEAD) {
            services[i].state = ST_WAITING;
            service_start(&services[i]);
            /* Wait for this service to complete */
            int status;
            while (services[i].pid > 0) {
                int wpid = waitpid(services[i].pid, &status, 0);
                if (wpid == services[i].pid) {
                    handle_child_exit(&services[i], status);
                    break;
                }
            }
        }
    }

    /* Phase 2: boot and bootwait */
    for (int i = 0; i < service_count; i++) {
        if (!(services[i].runlevels & rlmask))
            continue;
        if (services[i].action == ACT_BOOT && services[i].state == ST_DEAD) {
            service_start(&services[i]);
        }
        if (services[i].action == ACT_BOOTWAIT && services[i].state == ST_DEAD) {
            services[i].state = ST_WAITING;
            service_start(&services[i]);
            int status;
            while (services[i].pid > 0) {
                int wpid = waitpid(services[i].pid, &status, 0);
                if (wpid == services[i].pid) {
                    handle_child_exit(&services[i], status);
                    break;
                }
            }
        }
    }

    /* Phase 3: respawn services */
    for (int i = 0; i < service_count; i++) {
        if (!(services[i].runlevels & rlmask))
            continue;
        if (services[i].action == ACT_RESPAWN && services[i].state == ST_DEAD) {
            service_start(&services[i]);
        }
    }

    /* Phase 4: once services (fire-and-forget) */
    for (int i = 0; i < service_count; i++) {
        if (!(services[i].runlevels & rlmask))
            continue;
        if (services[i].action == ACT_ONCE && services[i].state == ST_DEAD) {
            service_start(&services[i]);
        }
    }
}

/* ── Runlevel switching (Item U4) ────────────────────────────────────── */

/*
 * Transition the system to a new runlevel.
 *
 * Semantics:
 *   0 = halt (system shutdown)
 *   1 = single-user (maintenance mode)
 *   2 = multi-user (default)
 *   3 = networking
 *   5 = graphical
 *   6 = reboot
 *
 * Implementation:
 *   1. Stop services that are running but NOT allowed in the new runlevel
 *   2. Start services that are allowed in the new runlevel but not running
 *   3. Update current_runlevel
 *   4. For runlevels 0 and 6, trigger shutdown/reboot after stopping services
 */
static void set_runlevel(int new_rl)
{
    if (new_rl < 0 || new_rl > 9) {
        puts("init: invalid runlevel ");
        put_dec(new_rl);
        puts("\n");
        return;
    }

    if (new_rl == current_runlevel) {
        puts("init: already in runlevel ");
        put_dec(new_rl);
        puts("\n");
        return;
    }

    puts("init: switching to runlevel ");
    put_dec(new_rl);
    puts("\n");

    unsigned int old_mask = runlevel_bitmask(current_runlevel);
    unsigned int new_mask = runlevel_bitmask(new_rl);
    int old_rl = current_runlevel;
    current_runlevel = new_rl;

    /* Phase 1: Stop services not allowed in the new runlevel */
    for (int i = 0; i < service_count; i++) {
        if (services[i].state == ST_RUNNING) {
            if (!(services[i].runlevels & new_mask)) {
                puts("init: stopping '");
                puts(services[i].id);
                puts("' for runlevel change\n");
                service_stop(&services[i]);
            }
        }
    }

    /* Reap any children that stopped */
    reap_children();

    /* Phase 2: Start services that are now allowed but not running */
    for (int i = 0; i < service_count; i++) {
        if (services[i].state == ST_DEAD &&
            (services[i].runlevels & new_mask) &&
            (services[i].action == ACT_RESPAWN ||
             services[i].action == ACT_BOOT ||
             services[i].action == ACT_BOOTWAIT ||
             services[i].action == ACT_WAIT)) {
            puts("init: starting '");
            puts(services[i].id);
            puts("' for runlevel ");
            put_dec(new_rl);
            puts("\n");
            service_start(&services[i]);
        }
    }

    /* Handle special runlevels */
    if (new_rl == 0) {
        puts("init: runlevel 0 -- halting system\n");
        shutdown_requested = 1;
    } else if (new_rl == 6) {
        puts("init: runlevel 6 -- rebooting system\n");
        shutdown_requested = 1;
    }

    (void)old_rl;
    (void)old_mask;
}

/* ── Init pipe: runlevel change notification (Item U4) ───────────────── */

/*
 * Check /var/run/initpipe for a pending runlevel change request.
 *
 * The kernel's 'init N' shell command writes a single ASCII digit to
 * this file.  Init reads it on each main-loop iteration and processes
 * any pending runlevel switch.  After processing, the file is truncated.
 *
 * If the file doesn't exist yet, it is created empty so that future
 * writes by the init command will succeed.
 */
static void check_initpipe(void)
{
    /* Open the pipe file (read-only) */
    int fd = open(INITPIPE_PATH, O_RDONLY);
    if (fd < 0) {
        /* File doesn't exist yet -- try to create it */
        fd = open(INITPIPE_PATH, O_WRONLY | O_CREAT);
        if (fd >= 0)
            close(fd);
        return;
    }

    /* Read one byte */
    char buf[2];
    long n = read(fd, buf, 1);
    close(fd);

    if (n <= 0)
        return;  /* empty or error */

    buf[1] = '\0';

    /* Validate: must be a single digit 0-9 */
    if (buf[0] >= '0' && buf[0] <= '9') {
        int new_rl = buf[0] - '0';
        set_runlevel(new_rl);
    }

    /* Truncate the file by reopening with O_TRUNC */
    fd = open(INITPIPE_PATH, O_WRONLY | O_TRUNC);
    if (fd >= 0)
        close(fd);
}

/* ── Console output helpers (no snprintf) ─────────────────────────────── */

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

/* ── Read /etc/hostname at boot (Item U23) ───────────────────────────── */

/*
 * Read the system hostname from /etc/hostname on boot and set it via
 * the sethostname() syscall.  Silently continues if the file does not
 * exist or is empty — the kernel default hostname (e.g. "(none)" or
 * "localhost") will be used.
 */
static void init_set_hostname(void)
{
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd < 0) {
        /* No hostname file — not an error, use kernel default */
        return;
    }

    char buf[65]; /* HOST_NAME_MAX is typically 64 */
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
        return; /* Empty file — nothing to set */

    /* Strip trailing whitespace/newline from the hostname */
    int end = (int)n - 1;
    while (end >= 0 && (buf[end] == '\n' || buf[end] == '\r' ||
                        buf[end] == ' ' || buf[end] == '\t'))
        end--;

    if (end < 0)
        return; /* All whitespace — ignore */

    buf[end + 1] = '\0';

    /* Set hostname via syscall */
    if (sethostname(buf, (size_t)(end + 1)) == 0) {
        puts("init: set hostname from /etc/hostname: ");
        puts(buf);
        puts("\n");
    }
}

/* ── Main ─────────────────────────────────────────────────────────────── */

void _start(void)
{
    /* Ignore SIGCHLD so terminated children don't become zombies.
     * We reap them explicitly via waitpid() in the main loop. */
    signal(SIGCHLD, SIGIGN);

    /* Log startup */
    puts("init: PID 1 starting\n");

    /* Read /etc/hostname and set kernel hostname (Item U23) */
    init_set_hostname();

    /* Parse /etc/inittab */
    init_parse_inittab();
    put_dec(service_count);
    puts(" services configured\n");

    /* Boot services */
    boot_services();
    puts("init: boot sequence complete, entering service loop\n");

    /* Main loop: reap children, check runlevel changes, and handle shutdown */
    while (!shutdown_requested) {
        reap_children();
        check_initpipe();

        /* Brief pause to avoid busy-waiting */
        for (volatile int i = 0; i < 100000; i++)
            __asm__ volatile("pause");
    }

    /* Shutdown: stop all services */
    puts("init: shutting down services...\n");
    for (int i = 0; i < service_count; i++) {
        if (services[i].pid > 0)
            service_stop(&services[i]);
    }

    /* Wait for all children to exit */
    for (int i = 0; i < 100; i++) {
        reap_children();
        int any_running = 0;
        for (int j = 0; j < service_count; j++) {
            if (services[j].pid > 0) {
                any_running = 1;
                break;
            }
        }
        if (!any_running)
            break;
        for (volatile int j = 0; j < 1000000; j++)
            __asm__ volatile("pause");
    }

    puts("init: goodbye\n");
    exit(0);
}
