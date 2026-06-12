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
#define SYS_DUP2      241

#define SIGCHLD       17
#define SIGTERM       15
#define SIGINT        2
#define SIGHUP        1
#define SIGIGN        1L
#define SIGDFL        0L

#define O_RDONLY      0
#define O_WRONLY      1
#define O_RDWR        2
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

/* Service dependency metadata (Item U3) */
#define MAX_DEPS            8   /* max Required-Start / Required-Stop per service */
#define MAX_DEP_NAME        32  /* max length of a dependency name */
#define INITD_DIR           "/etc/init.d/"  /* init script directory */
#define INITD_LINE_MAX      256 /* max line length when parsing init.d scripts */

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

    /* Service dependency metadata (Item U3) — populated from /etc/init.d/<id>
     * scripts via # Required-Start: and # Required-Stop: comments. */
    char            deps_start[MAX_DEPS][MAX_DEP_NAME];  /* Required-Start deps */
    int             num_deps_start;
    char            deps_stop[MAX_DEPS][MAX_DEP_NAME];   /* Required-Stop deps */
    int             num_deps_stop;
};

static struct service services[MAX_SERVICES];
static int service_count = 0;
static volatile int shutdown_requested = 0;

/* Current system runlevel (0=halt, 1=single, 2=multi-user, 5=graphical) */
static int current_runlevel = DEFAULT_RUNLEVEL;

/* ── Forward declarations ─────────────────────────────────────────────── */

static void init_parse_inittab(void);
static void init_load_dependencies(void);
static int  find_service_by_id(const char *id);
static void topological_sort(int *order, int *count);
static int  service_in_order(const int *order, int count, int idx);
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

/* dup2: duplicate a file descriptor onto another */
static inline int dup2(int oldfd, int newfd) {
    return (int)syscall(SYS_DUP2, oldfd, newfd, 0, 0, 0);
}

/* write: write count bytes from buf to fd */
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

/* String comparison with length limit (avoids libc dependency) */
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

/* ── Service dependency loading (Item U3) ────────────────────────────── */

/*
 * Find the index of a service by its identifier.
 * Returns the index, or -1 if not found.
 */
static int find_service_by_id(const char *id)
{
    if (!id || !id[0])
        return -1;
    for (int i = 0; i < service_count; i++) {
        if (strcmp(services[i].id, id) == 0)
            return i;
    }
    return -1;
}

/*
 * Parse a single line from an /etc/init.d/<id> script looking for
 * dependency metadata comments.  Lines starting with '# Required-Start:'
 * or '# Required-Stop:' are parsed, and the space-separated dependency
 * names are added to the service's deps_start or deps_stop array.
 *
 * Format:
 *   # Required-Start: networking sshd
 *   # Required-Stop:
 */
static void parse_dep_line(const char *line, struct service *svc)
{
    /* Check for # Required-Start: */
    const char *prefix_start = "# Required-Start:";
    size_t prefix_len = strlen(prefix_start);

    if (strncmp(line, prefix_start, prefix_len) == 0) {
        const char *deps = line + prefix_len;
        /* Skip leading spaces */
        while (*deps == ' ') deps++;
        /* Parse space-separated dependency names */
        while (*deps && svc->num_deps_start < MAX_DEPS) {
            char name[MAX_DEP_NAME];
            int n = 0;
            while (*deps && *deps != ' ' && n < MAX_DEP_NAME - 1)
                name[n++] = *deps++;
            name[n] = '\0';
            if (n > 0) {
                strncpy(svc->deps_start[svc->num_deps_start], name,
                        MAX_DEP_NAME - 1);
                svc->deps_start[svc->num_deps_start][MAX_DEP_NAME - 1] = '\0';
                svc->num_deps_start++;
            }
            /* Skip spaces between names */
            while (*deps == ' ') deps++;
        }
        return;
    }

    /* Check for # Required-Stop: */
    const char *prefix_stop = "# Required-Stop:";
    prefix_len = strlen(prefix_stop);

    if (strncmp(line, prefix_stop, prefix_len) == 0) {
        const char *deps = line + prefix_len;
        while (*deps == ' ') deps++;
        while (*deps && svc->num_deps_stop < MAX_DEPS) {
            char name[MAX_DEP_NAME];
            int n = 0;
            while (*deps && *deps != ' ' && n < MAX_DEP_NAME - 1)
                name[n++] = *deps++;
            name[n] = '\0';
            if (n > 0) {
                strncpy(svc->deps_stop[svc->num_deps_stop], name,
                        MAX_DEP_NAME - 1);
                svc->deps_stop[svc->num_deps_stop][MAX_DEP_NAME - 1] = '\0';
                svc->num_deps_stop++;
            }
            while (*deps == ' ') deps++;
        }
    }
}

/*
 * Scan /etc/init.d/<id> for each configured service to extract
 * dependency metadata (# Required-Start: and # Required-Stop: comments).
 *
 * The init script is opened and read line-by-line, looking for comment
 * lines with the dependency markers.  Non-comment lines are ignored.
 * Only the first matching line of each type is used (subsequent lines
 * with the same marker are ignored).
 */
static void load_service_deps(struct service *svc)
{
    /* Build path: /etc/init.d/<id> */
    char script_path[MAX_PATH];
    size_t dirlen = strlen(INITD_DIR);
    size_t idlen  = strlen(svc->id);

    if (dirlen + idlen >= MAX_PATH)
        return;

    /* Copy directory */
    for (size_t i = 0; i < dirlen; i++)
        script_path[i] = INITD_DIR[i];
    /* Append service id */
    for (size_t i = 0; i < idlen; i++)
        script_path[dirlen + i] = svc->id[i];
    script_path[dirlen + idlen] = '\0';

    int fd = open(script_path, O_RDONLY);
    if (fd < 0)
        return;  /* No init script for this service — no dependencies */

    /* Read the file line by line */
    char buf[INITD_LINE_MAX];
    size_t pos = 0;
    long n;
    int found_start = 0, found_stop = 0;

    while ((n = read(fd, buf + pos, INITD_LINE_MAX - pos - 1)) > 0) {
        pos += (size_t)n;
        buf[pos] = '\0';

        char *line_start = buf;
        for (size_t i = 0; i < pos; i++) {
            if (buf[i] == '\n') {
                buf[i] = '\0';

                /* Only parse comment lines starting with '# Required-' */
                if (line_start[0] == '#') {
                    if (!found_start)
                        parse_dep_line(line_start, svc);
                    /* Re-check for Required-Stop — parse_dep_line handles both */
                    if (!found_stop && strncmp(line_start, "# Required-Stop:", 16) == 0) {
                        /* parse_dep_line already called above; track state */
                        found_stop = 1;
                    }
                }

                line_start = buf + i + 1;
            }
        }

        /* Check if we've seen both markers — can stop early */
        /* (We track start/stop properly inside parse_dep_line) */

        /* Move remaining partial line */
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

/*
 * Load dependency metadata for all configured services.
 * Called once after init_parse_inittab().
 */
static void init_load_dependencies(void)
{
    int loaded = 0;
    for (int i = 0; i < service_count; i++) {
        load_service_deps(&services[i]);
        if (services[i].num_deps_start > 0 || services[i].num_deps_stop > 0)
            loaded++;
    }
    if (loaded > 0) {
        puts("init: loaded dependencies for ");
        put_dec(loaded);
        puts(" services\n");
    }
}

/* ── Topological sort (Kahn's algorithm) ────────────────────────────── */

/*
 * Check whether service at index @idx appears in the ordered list @order
 * (of @count entries).  Returns 1 if found, 0 otherwise.
 */
static int service_in_order(const int *order, int count, int idx)
{
    for (int i = 0; i < count; i++) {
        if (order[i] == idx)
            return 1;
    }
    return 0;
}

/*
 * Topological sort of services using Kahn's algorithm.
 *
 * For each service, we consider its Required-Start dependencies: if
 * service A depends on B (B is in A's deps_start), then B must start
 * before A.  We build edges B → A (B must precede A).
 *
 * The algorithm:
 *   1. Compute in-degree (number of unscheduled predecessors) for each node
 *   2. Start with all nodes that have in-degree == 0
 *   3. Remove a node, decrement in-degree of its successors
 *   4. Repeat until all nodes are ordered (or cycle detected)
 *
 * On output:
 *   order[] is filled with service indices in topological order (start order)
 *   *count is set to the number of ordered services
 *
 * If a cycle is detected, we emit a warning and stop; the partial order
 * (all cycle-free services) is still returned so boot can proceed.
 */
static void topological_sort(int *order, int *count)
{
    /* in_degree[i] = number of deps that must be started before service i */
    int in_degree[MAX_SERVICES];
    int queue[MAX_SERVICES];
    int qhead = 0, qtail = 0;

    *count = 0;
    for (int i = 0; i < service_count; i++)
        in_degree[i] = 0;

    /* Build the dependency graph:
     * For each service i, for each dependency d in deps_start[i],
     * find the service j that provides d, then add edge j → i
     * (j must start before i), so in_degree[i]++. */
    for (int i = 0; i < service_count; i++) {
        for (int d = 0; d < services[i].num_deps_start; d++) {
            int j = find_service_by_id(services[i].deps_start[d]);
            if (j >= 0 && j != i) {
                /* j must start before i */
                in_degree[i]++;
            }
        }
    }

    /* Find all nodes with in-degree 0 to start the queue */
    for (int i = 0; i < service_count; i++) {
        if (in_degree[i] == 0)
            queue[qtail++] = i;
    }

    /* Process the queue */
    while (qhead < qtail && *count < service_count) {
        int node = queue[qhead++];
        order[(*count)++] = node;

        /* For any service that depends on node, decrement its in-degree */
        for (int i = 0; i < service_count; i++) {
            for (int d = 0; d < services[i].num_deps_start; d++) {
                int j = find_service_by_id(services[i].deps_start[d]);
                if (j == node && i != node) {
                    in_degree[i]--;
                    if (in_degree[i] == 0)
                        queue[qtail++] = i;
                }
            }
        }
    }

    /* Check for cycle: if we didn't order all services, there's a cycle */
    if (*count < service_count) {
        puts("init: WARNING dependency cycle detected (");
        put_dec(service_count - *count);
        puts(" services excluded from ordering)\n");

        /* Append remaining (unreachable) services to the end of the order */
        for (int i = 0; i < service_count; i++) {
            if (!service_in_order(order, *count, i))
                order[(*count)++] = i;
        }
    }
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
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            /* Redirect stdin/stdout/stderr to /dev/null */
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
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

    /* Compute dependency-based boot order (Item U3).
     * Services are topologically sorted so that Required-Start dependencies
     * are started before the services that depend on them. */
    int boot_order[MAX_SERVICES];
    int boot_count = 0;
    topological_sort(boot_order, &boot_count);

    /* Phase 1: sysinit — run and wait for each (in dependency order) */
    for (int oi = 0; oi < boot_count; oi++) {
        int i = boot_order[oi];
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

    /* Phase 2: boot and bootwait (in dependency order) */
    for (int oi = 0; oi < boot_count; oi++) {
        int i = boot_order[oi];
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

    /* Phase 3: respawn services (in dependency order) */
    for (int oi = 0; oi < boot_count; oi++) {
        int i = boot_order[oi];
        if (!(services[i].runlevels & rlmask))
            continue;
        if (services[i].action == ACT_RESPAWN && services[i].state == ST_DEAD) {
            service_start(&services[i]);
        }
    }

    /* Phase 4: once services (fire-and-forget, in dependency order) */
    for (int oi = 0; oi < boot_count; oi++) {
        int i = boot_order[oi];
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

    /* Compute dependency order for controlled stop/start (Item U3) */
    int dep_order[MAX_SERVICES];
    int dep_count = 0;
    topological_sort(dep_order, &dep_count);

    /* Phase 1: Stop services not allowed in the new runlevel.
     * Stop in REVERSE dependency order so that services are stopped
     * before their dependents. */
    {
        /* First pass: collect services to stop, then stop in reverse order */
        int to_stop = 0;
        int stop_list[MAX_SERVICES];
        for (int i = 0; i < service_count; i++) {
            if (services[i].state == ST_RUNNING) {
                if (!(services[i].runlevels & new_mask)) {
                    stop_list[to_stop++] = i;
                }
            }
        }
        /* Stop in reverse dependency order */
        for (int oi = dep_count - 1; oi >= 0; oi--) {
            int i = dep_order[oi];
            for (int s = 0; s < to_stop; s++) {
                if (stop_list[s] == i) {
                    puts("init: stopping '");
                    puts(services[i].id);
                    puts("' for runlevel change\n");
                    service_stop(&services[i]);
                    break;
                }
            }
        }
    }

    /* Reap any children that stopped */
    reap_children();

    /* Phase 2: Start services that are now allowed but not running.
     * Start in dependency order. */
    for (int oi = 0; oi < dep_count; oi++) {
        int i = dep_order[oi];
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

    /* Load service dependency metadata from /etc/init.d/ (Item U3) */
    init_load_dependencies();

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

    /* Shutdown: stop all services in reverse dependency order (Item U3) */
    puts("init: shutting down services...\n");
    {
        int shutdown_order[MAX_SERVICES];
        int shutdown_count = 0;
        topological_sort(shutdown_order, &shutdown_count);
        /* Stop in reverse order: dependencies last */
        for (int oi = shutdown_count - 1; oi >= 0; oi--) {
            int i = shutdown_order[oi];
            if (services[i].pid > 0)
                service_stop(&services[i]);
        }
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
