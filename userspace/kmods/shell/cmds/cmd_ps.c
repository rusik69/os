/* cmd_ps.c — ps command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "string.h"

/* Signal names for state column */
static const char *sig_name(int state) {
    /* Map kernel process state to single-character Linux-style */
    switch (state) {
        case 0: return "?";  /* UNUSED */
        case 1: return "S";  /* READY (sleeping/interruptible) */
        case 2: return "R";  /* RUNNING */
        case 3: return "D";  /* BLOCKED (uninterruptible) */
        case 4: return "Z";  /* ZOMBIE */
        default: return "?";
    }
}

void cmd_ps(const char *args) {
    static struct libc_process_info procs[PROCESS_MAX];
    const char *state_names[] = { "UNUSED", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };

    /* Parse options */
    int show_all = 0;       /* -A or -e: all processes */
    int filter_pid = -1;    /* specific PID filter */
    const char *filter_user = NULL;
    int sort_mode = 0;      /* 0=none, 1=cpu, 2=mem */

    if (args) {
        const char *p = args;
        while (*p == ' ') p++;

        /* Check for --sort= flag (must come before other flags) */
        if (p[0] == '-' && p[1] == '-' && strncmp(p, "--sort=", 7) == 0) {
            const char *sort_val = p + 7;
            if (strncmp(sort_val, "cpu", 3) == 0)
                sort_mode = 1;
            else if (strncmp(sort_val, "mem", 3) == 0)
                sort_mode = 2;
            else
                kprintf("ps: unknown sort field '%s' (use cpu or mem)\n", sort_val);
            /* Skip past --sort=xxx */
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
        }

        if (*p == '-') {
            p++;
            while (*p && *p != ' ') {
                if (*p == 'A' || *p == 'e') {
                    show_all = 1;
                } else if (*p == 'u') {
                    p++;
                    while (*p == ' ') p++;
                    if (*p) {
                        filter_user = p;
                        /* Find end of username */
                        while (*p && *p != ' ') p++;
                    }
                    continue; /* already advanced past option char */
                }
                p++;
            }
        } else if (*p >= '0' && *p <= '9') {
            /* PID argument */
            filter_pid = 0;
            while (*p >= '0' && *p <= '9') {
                filter_pid = filter_pid * 10 + (*p - '0');
                p++;
            }
            if (filter_pid < 1) {
                kprintf("ps: invalid PID %d\n", filter_pid);
                return;
            }
        }
    }

    int n = libc_process_list(procs, PROCESS_MAX);

    /* Sort if requested */
    if (sort_mode == 1) {
        /* Sort by CPU (descending: cpu_user_ticks + cpu_system_ticks) */
        for (int i = 1; i < n; i++) {
            struct libc_process_info key = procs[i];
            uint64_t cpu_key = key.cpu_user_ticks + key.cpu_system_ticks;
            int j = i - 1;
            while (j >= 0) {
                uint64_t cpu_j = procs[j].cpu_user_ticks + procs[j].cpu_system_ticks;
                if (cpu_j >= cpu_key) break;
                procs[j + 1] = procs[j];
                j--;
            }
            procs[j + 1] = key;
        }
    } else if (sort_mode == 2) {
        /* Sort by memory (descending: max_rss) */
        for (int i = 1; i < n; i++) {
            struct libc_process_info key = procs[i];
            uint64_t mem_key = key.max_rss;
            int j = i - 1;
            while (j >= 0) {
                uint64_t mem_j = procs[j].max_rss;
                if (mem_j >= mem_key) break;
                procs[j + 1] = procs[j];
                j--;
            }
            procs[j + 1] = key;
        }
    }

    kprintf("PID  PPID PGID PRI S STATE    MODE   BG NAME\n");
    for (int i = 0; i < n; i++) {
        uint8_t st = procs[i].state;
        if (st > 4) st = 0;
        const char *state = procs[i].is_suspended ? "STOPPED" : state_names[st];
        char sig = sig_name(st)[0];

        /* Apply filters */
        if (filter_pid >= 0 && (int)procs[i].pid != filter_pid)
            continue;
        if (filter_user) {
            /* Simple check: we can't map PID to username without /proc/<pid>/status,
             * but we can skip filtering if we can't match. For now, show everything
             * if filtering by user is requested (username matching would need user DB). */
            /* If a user is specified, we try to match via /proc later */
            (void)filter_user;
        }

        kprintf("%-4u %-4u %-4u %-3u %c %-8s %-6s %-2s %s\n",
                procs[i].pid,
                procs[i].ppid,
                procs[i].pgid,
                procs[i].priority,
                sig,
                state,
                procs[i].is_user ? "user" : "kernel",
                procs[i].is_background ? "&" : "",
                procs[i].name);
    }
}
