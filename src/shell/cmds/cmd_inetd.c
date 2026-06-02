/*
 * cmd_inetd.c — inetd: Internet Super-Server Daemon (Item U9)
 *
 * Listens on multiple TCP ports (configured via /etc/inetd.conf) and
 * dispatches connections to the appropriate service handler. Supports
 * built-in services (echo, daytime, chargen, discard) as well as
 * external service execution via fork+exec.
 *
 * Inetd configuration format (/etc/inetd.conf):
 *   service-name socket-type protocol wait/nowait user executable args
 *
 * Built-in services use "internal" as the executable.
 *
 * Standard port assignments:
 *   echo    — 7/tcp   (RFC 862)
 *   discard — 9/tcp   (RFC 863)
 *   daytime — 13/tcp  (RFC 867)
 *   chargen — 19/tcp  (RFC 864)
 *
 * Usage:
 *   inetd              — start the daemon
 *   inetd stop         — stop the daemon
 *   inetd status       — show daemon status
 *   inetd reload       — reload /etc/inetd.conf
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"
#include "net.h"
#include "syscall.h"

/* ── Constants ──────────────────────────────────────────────────────── */

/* Maximum services we can manage */
#define INETD_MAX_SERVICES       16

/* Maximum line length in inetd.conf */
#define INETD_MAX_LINE           256

/* Maximum path length for executable */
#define INETD_MAX_PATH           64

/* Maximum service name length */
#define INETD_MAX_NAME           24

/* Accept timeout (in ticks) — short enough to remain responsive to stop */
#define INETD_ACCEPT_TIMEOUT     50   /* 0.5 seconds at 100 Hz */

/* Polling interval for the main loop */
#define INETD_POLL_INTERVAL      10   /* 0.1 seconds */

/* Buffer size for built-in service reads */
#define INETD_BUF_SIZE           1024

/* Config file path */
#define INETD_CONF_PATH          "/etc/inetd.conf"
#define INETD_LOG_PATH           "/var/log/inetd.log"

/* ── Service types ──────────────────────────────────────────────────── */

/* Protocol types */
enum inetd_proto {
    INETD_PROTO_TCP = 0,
    INETD_PROTO_UDP = 1
};

/* Wait/nowait flag — if wait, inetd waits for service to finish before
 * accepting another connection on this port (used for datagram services).
 * For TCP stream services, always "nowait". */
enum inetd_wait {
    INETD_NOWAIT = 0,
    INETD_WAIT   = 1
};

/* Built-in service types */
enum inetd_builtin {
    INETD_BUILTIN_NONE     = 0,
    INETD_BUILTIN_ECHO     = 1,   /* RFC 862 — echo back received data */
    INETD_BUILTIN_DISCARD  = 2,   /* RFC 863 — discard received data */
    INETD_BUILTIN_DAYTIME  = 3,   /* RFC 867 — return current date/time */
    INETD_BUILTIN_CHARGEN  = 4    /* RFC 864 — return character generator */
};

/* ── Service entry ──────────────────────────────────────────────────── */

struct inetd_service {
    int    active;           /* 1 = slot in use */
    char   name[INETD_MAX_NAME];  /* service name */
    uint16_t port;           /* port number */
    enum inetd_proto proto;  /* TCP or UDP */
    enum inetd_wait  wait;   /* wait/nowait */
    char   user[24];         /* user to run service as */
    enum inetd_builtin builtin;  /* built-in service type */
    char   executable[INETD_MAX_PATH];  /* path to executable */
    char   args[INETD_MAX_LINE];       /* command arguments */
    uint64_t conn_count;     /* total connections handled */
};

/* ── Daemon state ───────────────────────────────────────────────────── */

static volatile int inetd_stop_requested = 0;
static volatile int inetd_running       = 0;

/* Service table */
static struct inetd_service inetd_services[INETD_MAX_SERVICES];
static int inetd_num_services = 0;

/* Built-in port mapping for quick lookup */
static uint16_t builtin_ports[INETD_MAX_SERVICES];
static int builtin_port_count = 0;

/* ── Logging helper ─────────────────────────────────────────────────── */

static void inetd_log(const char *msg) {
    if (!msg) return;
    kprintf("[inetd] %s\n", msg);
    /* Ensure /var/log exists */
    libc_vfs_create("/var/log", 2);
    /* Append to log file */
    int fd = libc_syscall(SYS_OPEN, (uint64_t)INETD_LOG_PATH, 2, 0, 0, 0);
    if ((int)fd >= 0) {
        libc_lseek(fd, 0, 2); /* seek to end */
        libc_fd_write(fd, msg, (uint32_t)strlen(msg));
        libc_fd_write(fd, "\n", 1);
        libc_syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
    }
}

/* ── Built-in service handlers ──────────────────────────────────────── */

/* Echo service (RFC 862): send back whatever data is received */
static void inetd_handle_echo(int conn_id) {
    char buf[INETD_BUF_SIZE];
    for (;;) {
        int n = net_tcp_recv(conn_id, buf, sizeof(buf), 200);
        if (n <= 0) break;
        net_tcp_send(conn_id, buf, (uint16_t)n);
    }
}

/* Discard service (RFC 863): just read and discard */
static void inetd_handle_discard(int conn_id) {
    char buf[INETD_BUF_SIZE];
    for (;;) {
        int n = net_tcp_recv(conn_id, buf, sizeof(buf), 200);
        if (n <= 0) break;
        /* Data discarded */
    }
}

/* Daytime service (RFC 867): return current date/time as string */
static void inetd_handle_daytime(int conn_id) {
    char buf[64];
    uint64_t now = libc_time_seconds();

    /* Compute date/time from epoch seconds */
    uint64_t days = now / 86400;
    uint64_t remaining = now % 86400;
    int hour = (int)(remaining / 3600);
    int minute = (int)((remaining % 3600) / 60);
    int sec = (int)(remaining % 60);

    /* Day of week (epoch was Thursday = 4) */
    static const char *wday_names[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char *mon_names[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int wday = (int)((days + 4) % 7);

    /* Year */
    int y = 1970;
    while (1) {
        int leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        int days_in_year = leap ? 366 : 365;
        if ((uint64_t)days_in_year > days) break;
        days -= days_in_year;
        y++;
    }

    /* Month */
    static const int dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int m = 0;
    while (m < 11) {
        int d = dim[m];
        if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)))
            d = 29;
        if ((int)days < d) break;
        days -= d;
        m++;
    }
    int day = (int)(days + 1);

    /* Format daytime string: "Wed Jun 10 14:30:00 2026\n" */
    int len = snprintf(buf, sizeof(buf), "%s %s %2d %02d:%02d:%02d %04d\n",
                       wday_names[wday], mon_names[m], day,
                       hour, minute, sec, y);
    if (len > 0)
        net_tcp_send(conn_id, buf, (uint16_t)(len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1));
}

/* Chargen service (RFC 864): generate a repeating pattern of characters */
static void inetd_handle_chargen(int conn_id) {
    char line[76]; /* 72 printable chars + \r\n */
    char pattern[96];
    int pi = 0;

    /* Build the character set for cycling */
    for (char c = 32; c < 127; c++)
        pattern[pi++] = c;
    pattern[pi] = '\0';

    int start = 0;
    /* Generate 100 lines, then close */
    for (int l = 0; l < 100; l++) {
        int wi = 0;
        for (int j = 0; j < 72; j++) {
            line[wi++] = pattern[(start + j) % pi];
        }
        line[wi++] = '\r';
        line[wi++] = '\n';
        net_tcp_send(conn_id, line, (uint16_t)wi);
        start = (start + 1) % pi;
        /* Yield to allow other processing */
        if (l % 10 == 0) libc_sleep_ticks(1);
    }
}

/* ── External service handler — stub (for built-in only) ─────────────── */

/* External services are not directly supported for TCP passthrough in the
 * current kernel model. They should be implemented as separate daemons
 * (e.g., sshd, telnetd, httpd) that register their own listeners via
 * net_tcp_listen(). Inetd provides built-in services and can log
 * connection attempts for external services. */
static void inetd_handle_external(int conn_id, const struct inetd_service *svc) {
    (void)conn_id;
    if (!svc || !svc->active) return;

    char logmsg[INETD_MAX_LINE + 40];
    snprintf(logmsg, sizeof(logmsg),
             "[inetd] Connection on port %u → %s (external, use dedicated daemon)",
             (unsigned)svc->port, svc->name);
    inetd_log(logmsg);
    kprintf("[inetd] External service '%s' on port %u not handled "
            "(use dedicated daemon: sshd, telnetd, httpd, etc.)\n",
            svc->name, (unsigned)svc->port);
}

/* ── Connection dispatcher ──────────────────────────────────────────── */

/* Handle a single connection on a given port */
static void inetd_handle_connection(uint16_t port, int conn_id) {
    /* Find the service entry by port */
    struct inetd_service *svc = NULL;
    for (int i = 0; i < inetd_num_services; i++) {
        if (inetd_services[i].active && inetd_services[i].port == port) {
            svc = &inetd_services[i];
            break;
        }
    }

    if (!svc) {
        /* Unknown port — close immediately */
        net_tcp_close(conn_id);
        return;
    }

    svc->conn_count++;

    if (svc->builtin != INETD_BUILTIN_NONE) {
        /* Handle built-in service */
        switch (svc->builtin) {
        case INETD_BUILTIN_ECHO:
            inetd_handle_echo(conn_id);
            break;
        case INETD_BUILTIN_DISCARD:
            inetd_handle_discard(conn_id);
            break;
        case INETD_BUILTIN_DAYTIME:
            inetd_handle_daytime(conn_id);
            break;
        case INETD_BUILTIN_CHARGEN:
            inetd_handle_chargen(conn_id);
            break;
        default:
            break;
        }
    } else {
        /* External service — fork and exec */
        inetd_handle_external(conn_id, svc);
    }

    /* Close the connection after handling */
    net_tcp_close(conn_id);
}

/* ── Configuration parsing ──────────────────────────────────────────── */

/* Map built-in service name to type */
static enum inetd_builtin inetd_lookup_builtin(const char *name) {
    if (!name) return INETD_BUILTIN_NONE;
    if (strcmp(name, "echo") == 0)    return INETD_BUILTIN_ECHO;
    if (strcmp(name, "discard") == 0)  return INETD_BUILTIN_DISCARD;
    if (strcmp(name, "daytime") == 0)  return INETD_BUILTIN_DAYTIME;
    if (strcmp(name, "chargen") == 0)  return INETD_BUILTIN_CHARGEN;
    return INETD_BUILTIN_NONE;
}

/* Map built-in type to string */
static const char *inetd_builtin_name(enum inetd_builtin bt) {
    switch (bt) {
    case INETD_BUILTIN_ECHO:    return "echo";
    case INETD_BUILTIN_DISCARD: return "discard";
    case INETD_BUILTIN_DAYTIME: return "daytime";
    case INETD_BUILTIN_CHARGEN: return "chargen";
    default: return "external";
    }
}

/* Lookup standard port for a built-in service name */
static uint16_t inetd_default_port(const char *name) {
    if (!name) return 0;
    if (strcmp(name, "echo") == 0)    return 7;
    if (strcmp(name, "discard") == 0)  return 9;
    if (strcmp(name, "daytime") == 0)  return 13;
    if (strcmp(name, "chargen") == 0)  return 19;
    return 0;
}

/* Parse one inetd.conf line. Returns 1 if a valid service was added. */
static int inetd_parse_line(const char *line) {
    if (!line || !*line) return 0;

    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;

    /* Empty line or comment */
    if (*line == '\0' || *line == '#' || *line == '\n') return 0;

    if (inetd_num_services >= INETD_MAX_SERVICES) {
        kprintf("[inetd] Max services (%d) reached, extra line ignored\n",
                INETD_MAX_SERVICES);
        return 0;
    }

    /* Tokenise the line.
     * Format: service-name socket-type protocol wait/nowait user executable args
     * Example:
     *   echo    stream  tcp  nowait  root  internal
     *   daytime stream  tcp  nowait  root  internal
     *   ftp     stream  tcp  nowait  root  /usr/sbin/ftpd  ftpd -l
     */

    char buf[INETD_MAX_LINE];
    strncpy(buf, line, INETD_MAX_LINE - 1);
    buf[INETD_MAX_LINE - 1] = '\0';

    /* Remove trailing newline */
    int len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    /* Tokenise by whitespace */
    char *tokens[8];
    int ntokens = 0;
    char *p = buf;
    while (*p && ntokens < 8) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        tokens[ntokens++] = p;
        /* Advance to next whitespace */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p++ = '\0'; }
    }

    /* Minimum fields: service-name socket-type protocol wait/nowait user executable */
    if (ntokens < 6) return 0;

    struct inetd_service svc;
    memset(&svc, 0, sizeof(svc));

    /* Field 1: service-name */
    strncpy(svc.name, tokens[0], INETD_MAX_NAME - 1);
    svc.name[INETD_MAX_NAME - 1] = '\0';

    /* Field 2: socket-type (only "stream" for TCP is supported currently) */
    /* We only support "stream" (TCP) for now */
    if (strcmp(tokens[1], "stream") != 0) {
        /* Skip non-stream entries (dgram support could be added later) */
        return 0;
    }

    /* Field 3: protocol */
    if (strcmp(tokens[2], "tcp") == 0) {
        svc.proto = INETD_PROTO_TCP;
    } else if (strcmp(tokens[2], "udp") == 0) {
        svc.proto = INETD_PROTO_UDP;
        /* UDP not yet fully supported in accept mode */
        return 0;
    } else {
        /* Unknown protocol */
        return 0;
    }

    /* Field 4: wait/nowait */
    if (strcmp(tokens[3], "wait") == 0) {
        svc.wait = INETD_WAIT;
    } else {
        svc.wait = INETD_NOWAIT; /* default */
    }

    /* Field 5: user */
    strncpy(svc.user, tokens[4], sizeof(svc.user) - 1);
    svc.user[sizeof(svc.user) - 1] = '\0';

    /* Field 6: executable (or "internal" for built-in) */
    strncpy(svc.executable, tokens[5], INETD_MAX_PATH - 1);
    svc.executable[INETD_MAX_PATH - 1] = '\0';

    /* Check for built-in */
    svc.builtin = inetd_lookup_builtin(svc.name);
    if (strcmp(svc.executable, "internal") == 0 && svc.builtin == INETD_BUILTIN_NONE) {
        /* If executable is "internal" but not a known built-in, check name */
        svc.builtin = inetd_lookup_builtin(svc.name);
    }

    /* Determine port */
    svc.port = inetd_default_port(svc.name);

    /* Field 7+: arguments (optional, for external services) */
    if (ntokens >= 7) {
        /* Reconstruct args from remaining tokens */
        char args_buf[INETD_MAX_LINE];
        int ai = 0;
        for (int t = 6; t < ntokens && ai < INETD_MAX_LINE - 2; t++) {
            if (t > 6) args_buf[ai++] = ' ';
            const char *src = tokens[t];
            while (*src && ai < INETD_MAX_LINE - 2)
                args_buf[ai++] = *src++;
        }
        args_buf[ai] = '\0';
        strncpy(svc.args, args_buf, INETD_MAX_LINE - 1);
        svc.args[INETD_MAX_LINE - 1] = '\0';
    }

    svc.active = 1;

    /* Add to service table */
    inetd_services[inetd_num_services++] = svc;

    kprintf("[inetd] Registered: %s (port %u, %s)\n",
            svc.name, (unsigned)svc.port,
            inetd_builtin_name(svc.builtin));
    return 1;
}

/* ── Config file loading ────────────────────────────────────────────── */

/* Load /etc/inetd.conf */
static int inetd_load_config(void) {
    /* Reset service table */
    for (int i = 0; i < INETD_MAX_SERVICES; i++)
        inetd_services[i].active = 0;
    inetd_num_services = 0;
    builtin_port_count = 0;

    /* Read the config file */
    char buf[8192];
    uint32_t size = 0;

    int ret = libc_vfs_read(INETD_CONF_PATH, buf, sizeof(buf) - 1, &size);
    if (ret < 0 || size == 0) {
        /* Config file doesn't exist or is empty — use defaults */
        kprintf("[inetd] No %s found, using built-in defaults\n",
                INETD_CONF_PATH);
        return 0;
    }

    buf[size] = '\0';

    /* Parse line by line */
    int count = 0;
    char *line = buf;
    while (line && *line && inetd_num_services < INETD_MAX_SERVICES) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;

        char saved = *nl;
        *nl = '\0';

        if (inetd_parse_line(line))
            count++;

        *nl = saved;
        line = nl + 1;
    }

    return count;
}

/* ── Register listeners on all service ports ────────────────────────── */

/* Register TCP listeners for all configured services */
static void inetd_register_listeners(void) {
    for (int i = 0; i < inetd_num_services; i++) {
        if (!inetd_services[i].active) continue;
        if (inetd_services[i].proto != INETD_PROTO_TCP) continue;

        /* Register in accept-queue mode (no callbacks) */
        net_tcp_listen(inetd_services[i].port, NULL, NULL, NULL);
        kprintf("[inetd] Listening on port %u (%s)\n",
                (unsigned)inetd_services[i].port,
                inetd_services[i].name);
    }

    /* Track which ports have registered listeners */
    builtin_port_count = 0;
    for (int i = 0; i < inetd_num_services; i++) {
        if (inetd_services[i].active &&
            inetd_services[i].proto == INETD_PROTO_TCP) {
            builtin_ports[builtin_port_count++] = inetd_services[i].port;
        }
    }
}

/* Unregister all listeners */
static void inetd_unregister_listeners(void) {
    for (int i = 0; i < inetd_num_services; i++) {
        if (!inetd_services[i].active) continue;
        if (inetd_services[i].proto != INETD_PROTO_TCP) continue;
        net_tcp_unlisten(inetd_services[i].port);
    }
    builtin_port_count = 0;
}

/* ── Default configuration if no /etc/inetd.conf exists ─────────────── */

/* Create a default inetd.conf with standard built-in services */
static void inetd_create_default_config(void) {
    /* Define default built-in services */
    static const char *default_services[] = {
        "echo    stream  tcp  nowait  root  internal",
        "discard stream  tcp  nowait  root  internal",
        "daytime stream  tcp  nowait  root  internal",
        "chargen stream  tcp  nowait  root  internal",
        NULL
    };

    for (int i = 0; default_services[i]; i++) {
        inetd_parse_line(default_services[i]);
    }
}

/* ── Show status ────────────────────────────────────────────────────── */

static void inetd_show_status(void) {
    kprintf("inetd status:\n");
    kprintf("  Running: %s\n", inetd_running ? "yes" : "no");
    kprintf("  Services: %d\n", inetd_num_services);
    if (inetd_num_services == 0) {
        kprintf("  (no services configured)\n");
        return;
    }
    kprintf("  %-16s %-6s %-10s %-12s %s\n",
            "NAME", "PORT", "TYPE", "BUILTIN", "CONNECTIONS");
    for (int i = 0; i < inetd_num_services; i++) {
        struct inetd_service *s = &inetd_services[i];
        if (!s->active) continue;
        kprintf("  %-16s %-6u %-10s %-12s %llu\n",
                s->name,
                (unsigned)s->port,
                s->proto == INETD_PROTO_TCP ? "tcp" : "udp",
                inetd_builtin_name(s->builtin),
                (unsigned long long)s->conn_count);
    }
}

/* ── Main daemon loop ───────────────────────────────────────────────── */

static void inetd_run_daemon(void) {
    inetd_stop_requested = 0;
    inetd_running = 1;

    /* Load config */
    int n = inetd_load_config();
    if (n == 0) {
        /* No config loaded — use defaults */
        inetd_create_default_config();
    }

    /* Register TCP listeners */
    inetd_register_listeners();

    kprintf("[inetd] Internet super-server started with %d service(s)\n",
            inetd_num_services);
    inetd_log("inetd daemon started");

    /* Main accept loop */
    while (!inetd_stop_requested) {
        /* Poll each port for new connections */
        int handled = 0;

        for (int i = 0; i < builtin_port_count && !inetd_stop_requested; i++) {
            uint16_t port = builtin_ports[i];
            int conn_id = net_tcp_accept(port, 1); /* very short timeout */

            if (conn_id >= 0) {
                kprintf("[inetd] Accepted connection on port %u (conn=%d)\n",
                        (unsigned)port, conn_id);
                inetd_handle_connection(port, conn_id);
                handled++;
            }
        }

        /* If nothing happened, sleep briefly to yield the CPU */
        if (handled == 0) {
            libc_sleep_ticks(INETD_POLL_INTERVAL);
        }
    }

    /* Cleanup */
    inetd_unregister_listeners();
    inetd_running = 0;
    kprintf("[inetd] Daemon stopped\n");
    inetd_log("inetd daemon stopped");
}

/* ── Shell command entry point ──────────────────────────────────────── */

void cmd_inetd(const char *args) {
    /* Handle arguments */
    if (args && *args) {
        /* Skip leading whitespace */
        while (*args == ' ') args++;

        if (strcmp(args, "stop") == 0) {
            if (!inetd_running) {
                kprintf("inetd: not running\n");
                return;
            }
            inetd_stop_requested = 1;
            kprintf("inetd: stop requested\n");
            return;
        }

        if (strcmp(args, "status") == 0) {
            inetd_show_status();
            return;
        }

        if (strcmp(args, "reload") == 0) {
            if (!inetd_running) {
                kprintf("inetd: not running, start first\n");
                return;
            }
            kprintf("inetd: reloading configuration...\n");
            /* Reload config: unregister all, reload, re-register */
            inetd_unregister_listeners();
            inetd_load_config();
            inetd_register_listeners();
            kprintf("inetd: configuration reloaded (%d services)\n",
                    inetd_num_services);
            return;
        }

        if (strcmp(args, "help") == 0 || strcmp(args, "--help") == 0) {
            kprintf("Usage: inetd [stop|status|reload|help]\n");
            kprintf("  (no args)  — start the daemon\n");
            kprintf("  stop       — stop the daemon\n");
            kprintf("  status     — show daemon status\n");
            kprintf("  reload     — reload /etc/inetd.conf\n");
            kprintf("Config file: %s\n", INETD_CONF_PATH);
            kprintf("Built-in services:\n");
            kprintf("  echo (7), discard (9), daytime (13), chargen (19)\n");
            return;
        }

        /* Unknown argument */
        kprintf("inetd: unknown option '%s'\n", args);
        kprintf("Usage: inetd [stop|status|reload|help]\n");
        return;
    }

    /* No args — run the daemon */
    inetd_run_daemon();
}
