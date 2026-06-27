/* service.c — service registry: start/stop/log + watchdog health monitoring */

#include "service.h"
#include "fs.h"
#include "printf.h"
#include "string.h"
#include "kernel.h"
#include "rtc.h"
#include "timer.h"
#include "panic.h"
#include "process.h"

static struct service services[SERVICE_MAX];
static int nservices = 0;

/* ── Watchdog state ───────────────────────────────────────────────────────── */

/* Tracks the last tick when the watchdog check ran, to avoid busy-looping. */
static uint64_t watchdog_last_check = 0;
static int      watchdog_initialized = 0;

/* ── Filesystem directory structure ───────────────────────────────────────── */

/*
 * Prepare the standard directory tree on the filesystem.
 *
 *  /etc          — configuration files
 *  /etc/services — one line per service: "<name> <enabled>"
 *  /var          — variable data
 *  /var/log      — service log files
 *  /var/run      — PID / state files
 *  /tmp/www      — httpd document root (HTTPD_ROOT_DIR)
 *
 * Directories that already exist are silently skipped.
 */
void __init service_init(void) {
    static const char *const dirs[] = {
        "/etc",
        "/var",
        "/var/log",
        "/var/run",
        "/var/lib",
        "/run",
        "/tmp",
        "/tmp/www",
    };
    for (int i = 0; i < (int)ARRAY_SIZE(dirs); i++) {
        uint32_t sz; uint8_t tp;
        if (fs_stat(dirs[i], &sz, &tp) < 0)
            fs_create(dirs[i], FS_TYPE_DIR);
    }

    /* Write default /etc/services configuration (name enabled) */
    const char *svcconf = "/etc/services";
    {
        uint32_t sz; uint8_t tp;
        if (fs_stat(svcconf, &sz, &tp) < 0) {
            const char cfg[] =
                "# /etc/services — service configuration\n"
                "# Format: <name> <enabled>\n"
                "telnetd enabled\n"
                "httpd   enabled\n";
            fs_write_file(svcconf, cfg, strlen(cfg));
        }
    }

    /* Install cclib.h — standard library header for programs compiled with cc */
    const char *cclib_path = "/cclib.h";
    {
        uint32_t sz; uint8_t tp;
        if (fs_stat(cclib_path, &sz, &tp) < 0) {
            const char cclib[] =
                "/* cclib.h - standard library for programs compiled with the built-in cc */\n"
                "#ifndef CCLIB_H\n"
                "#define CCLIB_H\n"
                "\n"
                "/* Syscall numbers (OS-specific) */\n"
                "#define SYS_READ    0\n"
                "#define SYS_WRITE   1\n"
                "#define SYS_OPEN    2\n"
                "#define SYS_CLOSE   3\n"
                "#define SYS_EXIT    4\n"
                "#define SYS_GETPID  5\n"
                "#define SYS_UPTIME  13\n"
                "#define SYS_MALLOC  179\n"
                "#define SYS_FREE    180\n"
                "#define SYS_REALLOC 181\n"
                "#define SYS_CALLOC  182\n"
                "\n"
                "/* __syscall(nr, a1..a5) is a compiler builtin that emits the syscall instruction */\n"
                "\n"
                "static void exit(int code) {\n"
                "    __syscall(SYS_EXIT, code, 0, 0, 0, 0, 0);\n"
                "}\n"
                "\n"
                "static int write(int fd, const char *buf, int len) {\n"
                "    return (int)__syscall(SYS_WRITE, fd, (long)buf, len, 0, 0, 0);\n"
                "}\n"
                "\n"
                "static int read(int fd, char *buf, int len) {\n"
                "    return (int)__syscall(SYS_READ, fd, (long)buf, len, 0, 0, 0);\n"
                "}\n"
                "\n"
                "static void *malloc(int size) {\n"
                "    return (void *)__syscall(SYS_MALLOC, size, 0, 0, 0, 0, 0);\n"
                "}\n"
                "\n"
                "static void free(void *ptr) {\n"
                "    __syscall(SYS_FREE, (long)ptr, 0, 0, 0, 0, 0);\n"
                "}\n"
                "\n"
                "static void *realloc(void *ptr, int size) {\n"
                "    return (void *)__syscall(SYS_REALLOC, (long)ptr, size, 0, 0, 0, 0);\n"
                "}\n"
                "\n"
                "static int getpid(void) {\n"
                "    return (int)__syscall(SYS_GETPID, 0, 0, 0, 0, 0, 0);\n"
                "}\n"
                "\n"
                "static int uptime(void) {\n"
                "    return (int)__syscall(SYS_UPTIME, 0, 0, 0, 0, 0, 0);\n"
                "}\n"
                "\n"
                "static int strlen(const char *s) {\n"
                "    int n = 0;\n"
                "    while (s[n]) n++;\n"
                "    return n;\n"
                "}\n"
                "\n"
                "static int strcmp(const char *a, const char *b) {\n"
                "    while (*a && *a == *b) { a++; b++; }\n"
                "    return (int)((unsigned char)*a - (unsigned char)*b);\n"
                "}\n"
                "\n"
                "static void *memset(void *s, int c, int n) {\n"
                "    char *p = (char *)s;\n"
                "    while (n-- > 0) *p++ = (char)c;\n"
                "    return s;\n"
                "}\n"
                "\n"
                "static void *memcpy(void *dst, const void *src, int n) {\n"
                "    char *d = (char *)dst;\n"
                "    const char *s = (const char *)src;\n"
                "    while (n-- > 0) *d++ = *s++;\n"
                "    return dst;\n"
                "}\n"
                "\n"
                "static int memcmp(const void *a, const void *b, int n) {\n"
                "    const unsigned char *p = (const unsigned char *)a;\n"
                "    const unsigned char *q = (const unsigned char *)b;\n"
                "    while (n-- > 0) { if (*p != *q) return (int)*p - (int)*q; p++; q++; }\n"
                "    return 0;\n"
                "}\n"
                "\n"
                "static void putchar(int c) {\n"
                "    char ch = (char)c;\n"
                "    write(1, &ch, 1);\n"
                "}\n"
                "\n"
                "static void puts(const char *s) {\n"
                "    write(1, s, strlen(s));\n"
                "    putchar('\\n');\n"
                "}\n"
                "\n"
                "static void print(const char *s) {\n"
                "    write(1, s, strlen(s));\n"
                "}\n"
                "\n"
                "static int atoi(const char *s) {\n"
                "    int n = 0, neg = 0;\n"
                "    while (*s == ' ') s++;\n"
                "    if (*s == '-') { neg = 1; s++; }\n"
                "    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }\n"
                "    return neg ? -n : n;\n"
                "}\n"
                "\n"
                "static void itoa_buf(int v, char *buf, int base) {\n"
                "    char tmp[32]; int i = 0, neg = 0;\n"
                "    if (v < 0 && base == 10) { neg = 1; v = -v; }\n"
                "    if (v == 0) { tmp[i++] = '0'; }\n"
                "    while (v > 0) { int r = v % base; tmp[i++] = (char)(r < 10 ? '0'+r : 'a'+r-10); v /= base; }\n"
                "    if (neg) tmp[i++] = '-';\n"
                "    int j = 0; while (i > 0) buf[j++] = tmp[--i];\n"
                "    buf[j] = 0;\n"
                "}\n"
                "\n"
                "static void print_int(int n) {\n"
                "    char buf[32];\n"
                "    itoa_buf(n, buf, 10);\n"
                "    print(buf);\n"
                "}\n"
                "\n"
                "#endif /* CCLIB_H */\n";
            fs_write_file(cclib_path, cclib, strlen(cclib));
        }
    }

    /* Write a default index page for the HTTP server if not present */
    const char *index = "/index.html";
    {
        uint32_t sz; uint8_t tp;
        if (fs_stat(index, &sz, &tp) < 0) {
            const char html[] =
                "<html><body><h1>OS Web Server</h1>"
                "<p>This is the default page served by the built-in HTTP server.</p>"
                "</body></html>\n";
            fs_write_file(index, html, strlen(html));
        }
    }
}

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static void log_line(struct service *svc, const char *msg) {
    /* Format:  [HH:MM:SS] msg\n  */
    char line[128];
    int pos = 0;

    struct rtc_time t;
    rtc_get_time(&t);
    {
        /* [HH:MM:SS] */
        line[pos++] = '[';
        line[pos++] = '0' + t.hour / 10;
        line[pos++] = '0' + t.hour % 10;
        line[pos++] = ':';
        line[pos++] = '0' + t.minute / 10;
        line[pos++] = '0' + t.minute % 10;
        line[pos++] = ':';
        line[pos++] = '0' + t.second / 10;
        line[pos++] = '0' + t.second % 10;
        line[pos++] = ']';
        line[pos++] = ' ';
    }

    int mlen = strlen(msg);
    if (mlen > (int)(sizeof(line) - pos - 2))
        mlen = (int)(sizeof(line) - pos - 2);
    memcpy(line + pos, msg, mlen);
    pos += mlen;
    line[pos++] = '\n';

    fs_append(svc->log_path, line, pos);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int service_register(const char *name, int (*start)(void), void (*stop)(void)) {
    if (nservices >= SERVICE_MAX) return -1;
    if (service_find(name)) return -1; /* already registered */

    struct service *svc = &services[nservices++];
    strncpy(svc->name, name, SERVICE_NAME_MAX - 1);
    svc->name[SERVICE_NAME_MAX - 1] = '\0';
    svc->state = SERVICE_STOPPED;
    svc->start = start;
    svc->stop  = stop;

    /* Build log path: /var/log/<name>.log */
    strncpy(svc->log_path, "/var/log/", sizeof(svc->log_path) - 1);
    strncat(svc->log_path, name, sizeof(svc->log_path) - strlen(svc->log_path) - 5);
    strncat(svc->log_path, ".log", sizeof(svc->log_path) - strlen(svc->log_path) - 1);

    /* ── Initialise watchdog fields ── */
    svc->pid            = 0;
    svc->crash_count    = 0;
    svc->max_restarts   = SERVICE_DEFAULT_MAX_RESTARTS;
    svc->critical       = 0;
    svc->last_heartbeat = 0;
    svc->last_restart   = 0;

    /* ── Initialise dependency fields ── */
    svc->ndeps = 0;
    memset(svc->deps, 0, sizeof(svc->deps));

    return 0;
}

/* ── /etc/services config writer ──────────────────────────────────────────── */

/* Rewrite /etc/services to reflect current enable/disable state of all
 * registered services.  Called after every start/stop. */
static void write_etc_services(void) {
    /* Build file content in a local buffer */
    static char buf[512];
    int pos = 0;
    const char *hdr =
        "# /etc/services — service configuration\n"
        "# Format: <name> <enabled>\n";
    int hlen = strlen(hdr);
    if (hlen < (int)(sizeof(buf) - pos - 1)) {
        memcpy(buf + pos, hdr, hlen);
        pos += hlen;
    }
    for (int i = 0; i < nservices && pos < (int)(sizeof(buf) - 30); i++) {
        const char *state_str = (services[i].state == SERVICE_RUNNING)
                                ? "enabled\n" : "disabled\n";
        int nlen = strlen(services[i].name);
        memcpy(buf + pos, services[i].name, nlen);
        pos += nlen;
        buf[pos++] = ' ';
        int slen = strlen(state_str);
        memcpy(buf + pos, state_str, slen);
        pos += slen;
    }
    fs_write_file("/etc/services", buf, (uint32_t)pos);
}

int service_start(const char *name) {
    struct service *svc = service_find(name);
    if (!svc) { kprintf("service: unknown service '%s'\n", name); return -1; }
    if (svc->state == SERVICE_RUNNING) {
        kprintf("service: %s is already running\n", name);
        return -1;
    }

    /* ── Recursively start dependencies first (Item U3) ──────────── */
    for (int d = 0; d < svc->ndeps; d++) {
        struct service *dep = service_find(svc->deps[d]);
        if (!dep) {
            kprintf("[svc] %s: dependency '%s' not registered, skipping\n",
                    name, svc->deps[d]);
            continue;
        }
        if (dep->state != SERVICE_RUNNING) {
            kprintf("[svc] %s: starting dependency '%s' first\n",
                    name, svc->deps[d]);
            int dep_rc = service_start(svc->deps[d]);
            if (dep_rc != 0) {
                kprintf("[svc] %s: dependency '%s' failed (rc=%d), aborting\n",
                        name, svc->deps[d], dep_rc);
                log_line(svc, "dependency start failed, aborting");
                return -1;
            }
        }
    }

    int rc = svc->start();
    if (rc == 0) {
        svc->state = SERVICE_RUNNING;
        /* Reset watchdog counters on successful manual start */
        svc->crash_count = 0;
        svc->last_heartbeat = timer_get_ticks();
        log_line(svc, "started");
        write_etc_services();
        kprintf("[svc] %s started\n", name);
    } else {
        log_line(svc, "start failed");
        kprintf("[svc] %s failed to start (rc=%lld)\n", name, (long long)rc);
    }
    return rc;
}

int service_stop(const char *name) {
    struct service *svc = service_find(name);
    if (!svc) { kprintf("service: unknown service '%s'\n", name); return -1; }
    if (svc->state == SERVICE_STOPPED) {
        kprintf("service: %s is already stopped\n", name);
        return -1;
    }

    /* ── Stop dependents first (reverse dependency order, Item U3) ─ */
    for (int i = 0; i < nservices; i++) {
        if (i == (int)(svc - services) || services[i].state != SERVICE_RUNNING)
            continue;
        for (int d = 0; d < services[i].ndeps; d++) {
            if (strcmp(services[i].deps[d], name) == 0) {
                kprintf("[svc] %s: stopping dependent '%s' first\n",
                        name, services[i].name);
                service_stop(services[i].name);
                break;
            }
        }
    }

    svc->stop();
    svc->state = SERVICE_STOPPED;
    svc->crash_count = 0;
    log_line(svc, "stopped");
    write_etc_services();
    kprintf("[svc] %s stopped\n", name);
    return 0;
}

int service_count(void) { return nservices; }

struct service *service_get(int idx) {
    return 0;
    return &services[idx];
}

struct service *service_find(const char *name) {
    for (int i = 0; i < nservices; i++)
        if (strcmp(services[i].name, name) == 0) return &services[i];
    return NULL;
}

void service_log(const char *name, const char *msg) {
    struct service *svc = service_find(name);
    if (svc) log_line(svc, msg);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ── Dependency Management (Item U3) ─────────────────────────────────────
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Add a dependency: service 'name' depends on 'dep' (Required-Start). */
int service_add_dep(const char *name, const char *dep) {
    struct service *svc = service_find(name);
    if (!svc) return -1;
    if (!dep || !*dep) return -1;
    if (svc->ndeps >= SERVICE_DEPS_MAX) return -1;

    /* Avoid duplicate entries */
    for (int i = 0; i < svc->ndeps; i++) {
        if (strcmp(svc->deps[i], dep) == 0)
            return 0; /* already present */
    }

    strncpy(svc->deps[svc->ndeps], dep, SERVICE_NAME_MAX - 1);
    svc->deps[svc->ndeps][SERVICE_NAME_MAX - 1] = '\0';
    svc->ndeps++;
    return 0;
}

/* Return the number of dependencies for a service, or -1 if not found. */
int service_num_deps(const char *name) {
    struct service *svc = service_find(name);
    if (!svc) return -1;
    return svc->ndeps;
}

/* Get the i-th dependency name for a service.  Returns NULL on error. */
const char *service_get_dep(const char *name, int i) {
    struct service *svc = service_find(name);
    if (!svc) return NULL;
    if (i < 0 || i >= svc->ndeps) return NULL;
    return svc->deps[i];
}

/*
 * Topological sort of services by dependency order (Kahn's algorithm).
 *
 * Fills 'order' with service indices such that if service A depends on
 * service B, then B appears before A in the output.  This means callers
 * should start services in order[0], order[1], ... order[n-1] and stop
 * in reverse.
 *
 * Returns the number of entries placed in 'order', or -1 if a dependency
 * cycle is detected.
 */
int service_sort_deps(int *order, int max_order) {
    /* Temporary copy of ndeps so we can decrement without modifying originals */
    int remaining[SERVICE_MAX];
    int queue[SERVICE_MAX];
    int qhead = 0, qtail = 0;
    int sorted = 0;

    if (max_order < nservices) return -1;

    /* Initialise */
    for (int i = 0; i < nservices; i++) {
        remaining[i] = services[i].ndeps;

        /* Services with no deps go straight into the queue */
        if (remaining[i] == 0) {
            queue[qtail++] = i;
        }
    }

    /* Process the queue */
    while (qhead < qtail && sorted < max_order) {
        int idx = queue[qhead++];
        order[sorted++] = idx;

        /* Find all services that depend on this one and decrement their count */
        for (int i = 0; i < nservices; i++) {
            if (i == idx || remaining[i] <= 0) continue;
            for (int d = 0; d < services[i].ndeps; d++) {
                if (strcmp(services[i].deps[d], services[idx].name) == 0) {
                    remaining[i]--;
                    if (remaining[i] == 0) {
                        queue[qtail++] = i;
                    }
                    break;
                }
            }
        }
    }

    /* If we didn't sort all services, there's a cycle or missing deps */
    if (sorted < nservices) {
        kprintf("[svc] dependency cycle detected! sorted=%d total=%d\n",
                sorted, nservices);
        for (int i = 0; i < nservices; i++) {
            if (remaining[i] > 0) {
                kprintf("[svc]   '%s' still waiting on %d deps\n",
                        services[i].name, remaining[i]);
            }
        }
        return -1;
    }

    return sorted;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ── Watchdog Health Monitoring (Item U6) ─────────────────────────────────
 * ═══════════════════════════════════════════════════════════════════════════ */

void service_set_pid(const char *name, int pid) {
    struct service *svc = service_find(name);
    if (svc) svc->pid = pid;
}

void service_set_critical(const char *name, int critical) {
    struct service *svc = service_find(name);
    if (svc) svc->critical = critical ? 1 : 0;
}

void service_heartbeat(const char *name) {
    struct service *svc = service_find(name);
    if (svc && svc->state == SERVICE_RUNNING)
        svc->last_heartbeat = timer_get_ticks();
}

void service_watchdog_init(void) {
    watchdog_last_check = 0;

    /* Set reasonable defaults for all existing services */
    for (int i = 0; i < nservices; i++) {
        services[i].max_restarts = SERVICE_DEFAULT_MAX_RESTARTS;
        services[i].pid          = 0;
        services[i].crash_count  = 0;
        services[i].last_heartbeat = timer_get_ticks();
        services[i].last_restart   = 0;
    }

    watchdog_initialized = 1;
    kprintf("[OK] service watchdog initialized (check every %u ticks)\n",
            (unsigned)SERVICE_WATCHDOG_INTERVAL_TICKS);
}

/*
 * Check whether a running service appears to be alive.
 *
 * For process-based services (pid > 0): verify the PID still exists by
 * checking the process table.
 *
 * For kernel services (pid == 0): check the heartbeat timestamp.  A
 * healthy service periodically calls service_heartbeat(); if the last
 * heartbeat is too old, we assume the service is stuck or deadlocked.
 *
 * Returns 1 if the service appears alive, 0 if it appears crashed/stale.
 */
static int service_is_alive(struct service *svc) {
    /* Stopped services are not expected to be alive */
    if (svc->state != SERVICE_RUNNING)
        return 1; /* not our concern */

    /* ── Process-based service: check PID existence ── */
    if (svc->pid > 0) {
        struct process *table = process_get_table();
        int found = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state != PROCESS_UNUSED && (int)table[i].pid == svc->pid) {
                found = 1;
                break;
            }
        }
        return found;
    }

    /* ── Kernel service: check heartbeat freshness ── */
    if (svc->last_heartbeat > 0) {
        uint64_t now = timer_get_ticks();
        uint64_t elapsed = now - svc->last_heartbeat;
        if (elapsed > SERVICE_HEARTBEAT_TIMEOUT_TICKS)
            return 0; /* heartbeat timeout — service is stale */
    }

    return 1; /* no heartbeat tracking — assume alive */
}

/*
 * Attempt to restart a crashed service with rate-limiting.
 *
 * Returns 0 on success, -1 if escalation was triggered.
 */
static int service_restart_crashed(struct service *svc) {
    uint64_t now = timer_get_ticks();

    /* Enforce cooldown between restart attempts */
    if (svc->last_restart > 0) {
        uint64_t since_restart = now - svc->last_restart;
        if (since_restart < SERVICE_RESTART_COOLDOWN_TICKS)
            return -1; /* too soon — skip */
    }

    /* ── Check restart limit ── */
    svc->crash_count++;
    svc->last_restart = now;

    kprintf("[svc-watchdog] %s: crash #%d (limit: %d/%s)\n",
            svc->name,
            svc->crash_count,
            svc->max_restarts,
            svc->critical ? "critical" : "non-critical");

    log_line(svc, "watchdog detected crash/stale state");

    if (svc->crash_count > svc->max_restarts) {
        /* ── Escalation: too many restarts ── */
        if (svc->critical) {
            /* Critical service keeps crashing — panic with full state capture */
            kprintf("[svc-watchdog] *** CRITICAL SERVICE '%s' KEEPS CRASHING ***\n"
                    "    crash_count=%d max_restarts=%d\n"
                    "    ==> PANIC ESCALATION\n",
                    svc->name, svc->crash_count, svc->max_restarts);
            log_line(svc, "CRITICAL ESCALATION — panic");
            panic("SERVICE WATCHDOG: critical service '%s' exceeded restart limit",
                  svc->name);
            /* not reached */
        }

        /* Non-critical: log and stop trying */
        kprintf("[svc-watchdog] %s: exceeded restart limit (%d). Giving up.\n",
                svc->name, svc->max_restarts);
        svc->state = SERVICE_STOPPED;
        log_line(svc, "watchdog: max restarts exceeded, service stopped");
        return -1;
    }

    /* ── Attempt restart ── */
    kprintf("[svc-watchdog] %s: attempting restart (attempt %d/%d)...\n",
            svc->name, svc->crash_count, svc->max_restarts);

    int rc = svc->start();
    if (rc == 0) {
        svc->state = SERVICE_RUNNING;
        svc->last_heartbeat = timer_get_ticks();
        kprintf("[svc-watchdog] %s: restart successful\n", svc->name);
        log_line(svc, "watchdog: auto-restart successful");
        return 0;
    }

    kprintf("[svc-watchdog] %s: restart FAILED (rc=%lld)\n", svc->name, (long long)rc);
    log_line(svc, "watchdog: auto-restart FAILED");
    return -1;
}

/*
 * Periodic health check for all registered services.
 *
 * Intended to be called from a kernel timer callback or the idle loop at
 * a moderate frequency (every ~3 seconds, controlled by the caller).
 *
 * For each running service:
 *   1. Check PID existence (process services) or heartbeat freshness
 *   2. If dead/stale, attempt auto-restart
 *   3. Escalate to panic for critical services that keep crashing
 */
void service_watchdog_check(void) {
    if (!watchdog_initialized)
        return;

    uint64_t now = timer_get_ticks();

    /* Rate-limit: only run every SERVICE_WATCHDOG_INTERVAL_TICKS */
    if (watchdog_last_check > 0) {
        uint64_t elapsed = now - watchdog_last_check;
        if (elapsed < SERVICE_WATCHDOG_INTERVAL_TICKS)
            return;
    }
    watchdog_last_check = now;

    /* Scan all services */
    for (int i = 0; i < nservices; i++) {
        struct service *svc = &services[i];

        if (svc->state != SERVICE_RUNNING)
            continue;

        if (!service_is_alive(svc)) {
            /* Service appears crashed/stale — attempt recovery */
            service_restart_crashed(svc);
        }
    }
}

/* ── Stub: service_unregister ─────────────────────────────── */
int service_unregister(const char *name)
{
    (void)name;
    kprintf("[service] service_unregister: not yet implemented\n");
    return 0;
}
