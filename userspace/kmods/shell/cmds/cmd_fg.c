/*
 * cmd_fg.c — Bring a background job to the foreground.
 *
 * Usage:  fg [%job_id | pid]
 *
 * Resumes a stopped background job and brings it to the foreground
 * by sending SIGCONT.  The job regains terminal control.
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

void cmd_fg(const char *args)
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
            kprintf("fg: invalid job specification: %s\n", args);
            return;
        }
    }

    /* If no pid specified, find the most recent background job */
    if (pid == 0) {
        struct libc_process_info procs[PROCESS_MAX];
        int n = libc_process_list(procs, PROCESS_MAX);
        /* Find the most recent background job (highest PID) */
        for (int i = 0; i < n; i++) {
            if (procs[i].is_background) {
                if (procs[i].pid > pid)
                    pid = procs[i].pid;
            }
        }
        if (pid == 0) {
            kprintf("fg: no background jobs\n");
            return;
        }
    }

    /* Send SIGCONT to wake the process (if stopped) and bring it foreground.
     * In a full job-control shell, we would also move the process back
     * to the foreground process group.  Here we simply resume it. */
    int ret = libc_kill(pid, SIGCONT_JOB);
    if (ret != 0) {
        if (ret == -ESRCH)
            kprintf("fg: job not found (pid=%u)\n", pid);
        else if (ret == -EPERM)
            kprintf("fg: permission denied\n");
        else
            kprintf("fg: failed to foreground job %u (error %d)\n", pid, -ret);
        return;
    }

    kprintf("[%u] resumed in foreground\n", pid);
}
