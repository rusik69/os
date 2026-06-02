/* cmd_service.c — 'service' shell command: start/stop/restart/reload/status/list
 *
 * Manages services registered via the kernel's service infrastructure.
 * Supports: start, stop, restart, reload, status, list, log
 */

#include "shell_cmds.h"
#include "service.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "libc.h"
#include "heap.h"

/*
 * Print a service's entry in the list output.
 * Combines status, PID, and crash count into a compact line.
 */
static void print_service_line(struct service *svc, int verbose)
{
    const char *state_str = "stopped";
    switch (svc->state) {
    case SERVICE_RUNNING:
        state_str = "running";
        break;
    case SERVICE_CRASHED:
        state_str = "crashed";
        break;
    default:
        break;
    }

    if (verbose) {
        /* Verbose: show name, state, PID, crashes, critical flag */
        kprintf("%-16s  %-8s  pid=%-5d  crashes=%d  %s\n",
                svc->name, state_str,
                svc->pid,
                svc->crash_count,
                svc->critical ? "CRITICAL" : "");
    } else {
        kprintf("%-16s  %s\n", svc->name, state_str);
    }
}

void cmd_service(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: service <start|stop|restart|reload|status|list|log> [name]\n");
        return;
    }

    /* Parse subcommand and optional service name */
    char subcmd[16] = {0};
    char name[SERVICE_NAME_MAX] = {0};

    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(subcmd) - 1)
        subcmd[i++] = *p++;
    while (*p == ' ') p++;
    i = 0;
    while (*p && i < (int)sizeof(name) - 1)
        name[i++] = *p++;

    /* ── list — show all registered services ──────────────────────── */
    if (strcmp(subcmd, "list") == 0) {
        int n = service_count();
        if (n == 0) {
            kprintf("No services registered.\n");
            return;
        }

        /* Check if verbose flag is set (e.g. "service list -v") */
        int verbose = 0;
        if (name[0] == '-' && name[1] == 'v')
            verbose = 1;

        kprintf("%-16s  STATUS      %s\n", "NAME", verbose ? "DETAILS" : "");
        kprintf("%-16s  ------      %s\n", "----", verbose ? "-------" : "");
        for (int j = 0; j < n; j++) {
            struct service *svc = service_get(j);
            if (svc)
                print_service_line(svc, verbose);
        }
        return;
    }

    /* ── status — show service status ─────────────────────────────── */
    if (strcmp(subcmd, "status") == 0) {
        if (*name == '\0') {
            /* No name → same as list */
            cmd_service("list");
            return;
        }
        struct service *svc = service_find(name);
        if (!svc) {
            kprintf("service: unknown service '%s'\n", name);
            return;
        }
        const char *state_str = "stopped";
        switch (svc->state) {
        case SERVICE_RUNNING: state_str = "running";  break;
        case SERVICE_CRASHED: state_str = "crashed";  break;
        }
        kprintf("%-16s  %s  (pid=%d, crashes=%d, %s)\n",
                svc->name, state_str,
                svc->pid, svc->crash_count,
                svc->critical ? "critical" : "non-critical");
        return;
    }

    /* ── start — start a service ──────────────────────────────────── */
    if (strcmp(subcmd, "start") == 0) {
        if (*name == '\0') {
            kprintf("service start: missing name\n");
            return;
        }
        service_start(name);
        return;
    }

    /* ── stop — stop a service ────────────────────────────────────── */
    if (strcmp(subcmd, "stop") == 0) {
        if (*name == '\0') {
            kprintf("service stop: missing name\n");
            return;
        }
        service_stop(name);
        return;
    }

    /* ── restart — stop then start a service ──────────────────────── */
    if (strcmp(subcmd, "restart") == 0) {
        if (*name == '\0') {
            kprintf("service restart: missing name\n");
            return;
        }
        kprintf("[svc] %s: restarting...\n", name);
        service_stop(name);
        /* Brief settle time for the stop to complete */
        for (volatile int j = 0; j < 500000; j++)
            __asm__ volatile("pause");
        service_start(name);
        return;
    }

    /* ── reload — signal a service to reload configuration ────────────
     * For kernel services, this does stop+start.  For process-based
     * services, a SIGHUP could be sent instead.  We use stop+start
     * as the universal approach since not all services support SIGHUP. */
    if (strcmp(subcmd, "reload") == 0) {
        if (*name == '\0') {
            kprintf("service reload: missing name\n");
            return;
        }
        struct service *svc = service_find(name);
        if (!svc) {
            kprintf("service: unknown service '%s'\n", name);
            return;
        }

        /* For process-based services, try SIGHUP first */
        if (svc->pid > 0 && svc->state == SERVICE_RUNNING) {
            libc_kill((uint32_t)svc->pid, 1); /* SIGHUP = 1 */
            kprintf("[svc] %s: SIGHUP sent (pid %d)\n", name, svc->pid);
        } else {
            /* Kernel service: stop + start */
            kprintf("[svc] %s: reloading (stop+start)...\n", name);
            service_stop(name);
            for (volatile int j = 0; j < 500000; j++)
                __asm__ volatile("pause");
            service_start(name);
        }
        return;
    }

    /* ── log — show last N lines of service log ───────────────────── */
    if (strcmp(subcmd, "log") == 0) {
        if (*name == '\0') {
            kprintf("service log: missing name\n");
            return;
        }
        /* Parse optional line count */
        int n_lines = 10;  /* default */
        char count_str[8] = {0};
        const char *p2 = args;
        /* Find the name in the original args */
        while (*p2 && *p2 != ' ') p2++;
        while (*p2 == ' ') p2++;
        /* Now p2 points to name; skip name to find optional count */
        while (*p2 && *p2 != ' ') p2++;
        while (*p2 == ' ') p2++;
        if (*p2) {
            i = 0;
            while (*p2 && i < (int)sizeof(count_str) - 1)
                count_str[i++] = *p2++;
            if (count_str[0])
                n_lines = atoi(count_str);
        }
        if (n_lines < 1) n_lines = 1;
        if (n_lines > 100) n_lines = 100;

        /* Read log file */
        char log_path[48];
        snprintf(log_path, sizeof(log_path), "/var/log/%s.log", name);

        uint8_t type;
        uint32_t size;
        if (fs_stat(log_path, &size, &type) < 0) {
            kprintf("service: no log for '%s' (file not found)\n", name);
            return;
        }
        if (size == 0) {
            kprintf("service: log for '%s' is empty\n", name);
            return;
        }

        /* Read the entire file */
        char *buf = (char *)kmalloc(size + 1);
        if (!buf) {
            kprintf("service: out of memory reading log\n");
            return;
        }

        uint32_t bytes_read = 0;
        if (fs_read_file(log_path, buf, size, &bytes_read) < 0 || bytes_read == 0) {
            kprintf("service: cannot read log for '%s'\n", name);
            kfree(buf);
            return;
        }
        buf[bytes_read] = '\0';

        /* Print last n_lines */
        int lines_printed = 0;
        int pos = (int)size - 1;

        /* Count backwards to find the starting position */
        while (pos >= 0 && lines_printed < n_lines) {
            if (buf[pos] == '\n')
                lines_printed++;
            pos--;
        }
        pos += 2;  /* Move past the newline */
        if (pos < 0) pos = 0;

        kprintf("--- /var/log/%s.log (last %d of %u lines) ---\n",
                name, n_lines, size);
        kprintf("%s", buf + pos);
        if (pos > 0 && buf[size-1] != '\n')
            kprintf("\n");

        kfree(buf);
        return;
    }

    kprintf("service: unknown subcommand '%s'\n", subcmd);
    kprintf("Usage: service <start|stop|restart|reload|status|list|log> [name]\n");
}
