/*
 * cmd_bg.c — Resume a stopped job in the background.
 *
 * Usage:  bg [%job_id | pid]
 *
 * Sends SIGCONT to the specified job/process and marks it as
 * a background process so it continues executing without
 * terminal control.
 *
 * Corresponds to POSIX job control (Item 313).
 */
#include "shell_cmds.h"
#include "libc.h"
#include "string.h"
#include "printf.h"
#include "errno.h"

/* Signal numbers for job control (not in allowed headers for app boundary).
 * SIGCONT = 18, SIGTSTP = 20 — from POSIX signal.h */
#define SIGCONT_JOB   18

void cmd_bg(const char *args)
{
    uint32_t pid = 0;

    if (args && *args) {
        const char *p = args;
        while (*p == ' ') p++;

        /* Parse %job_id or plain pid */
        if (*p == '%') {
            p++; /* skip '%' */
        }

        /* Parse numeric pid/job id */
        while (*p >= '0' && *p <= '9') {
            pid = pid * 10 + (uint32_t)(*p++ - '0');
        }

        if (pid == 0) {
            kprintf("bg: invalid job specification: %s\n", args);
            return;
        }
    }

    /* If no pid specified, find the most recent stopped background job */
    if (pid == 0) {
        struct libc_process_info procs[PROCESS_MAX];
        int n = libc_process_list(procs, PROCESS_MAX);
        /* Find the most recent stopped background job (highest PID) */
        for (int i = 0; i < n; i++) {
            if (procs[i].is_background && procs[i].is_suspended) {
                if (procs[i].pid > pid)
                    pid = procs[i].pid;
            }
        }
        if (pid == 0) {
            kprintf("bg: no stopped background jobs\n");
            return;
        }
    }

    /* Send SIGCONT to the process */
    int ret = libc_kill(pid, SIGCONT_JOB);
    if (ret != 0) {
        if (ret == -ESRCH)
            kprintf("bg: job not found (pid=%u)\n", pid);
        else if (ret == -EPERM)
            kprintf("bg: permission denied\n");
        else
            kprintf("bg: failed to resume job %u (error %d)\n", pid, -ret);
        return;
    }

    kprintf("[%u] resumed in background\n", pid);
}
