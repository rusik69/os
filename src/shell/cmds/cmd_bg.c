/* cmd_bg.c — resume a stopped background job */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

static int select_job(const char *args, uint32_t *pid, uint32_t *pgid) {
    struct libc_process_info procs[PROCESS_MAX];
    int n = libc_process_list(procs, PROCESS_MAX);

    while (args && *args == ' ') args++;
    if (!args || !*args) {
        for (int i = n - 1; i >= 0; i--) {
            if (procs[i].is_background) {
                *pid = procs[i].pid;
                *pgid = procs[i].pgid;
                return 0;
            }
        }
        return -1;
    }

    if (*args == '%') {
        int job = 0;
        args++;
        while (*args >= '0' && *args <= '9') { job = job * 10 + (*args - '0'); args++; }
        if (job <= 0) return -1;
        int seen = 0;
        for (int i = 0; i < n; i++) {
            if (!procs[i].is_background) continue;
            seen++;
            if (seen == job) {
                *pid = procs[i].pid;
                *pgid = procs[i].pgid;
                return 0;
            }
        }
        return -1;
    }

    if (!(*args >= '0' && *args <= '9')) return -1;
    *pid = 0;
    while (*args >= '0' && *args <= '9') { *pid = *pid * 10 + (*args - '0'); args++; }
    for (int i = 0; i < n; i++) {
        if (procs[i].pid == *pid) {
            *pgid = procs[i].pgid;
            return 0;
        }
    }
    *pgid = *pid;
    return 0;
}

void cmd_bg(const char *args) {
    uint32_t pid = 0;
    uint32_t pgid = 0;
    if (select_job(args, &pid, &pgid) != 0) {
        kprintf("Usage: bg [pid|%%job]\n");
        return;
    }
    if (pgid && libc_killpg(pgid, 18) == 0) {
        kprintf("[%u] Continued\n", (unsigned int)pid);
        return;
    }
    if (libc_kill(pid, 18) == 0) {
        kprintf("[%u] Continued\n", (unsigned int)pid);
        return;
    }
    kprintf("No such job: %u\n", (unsigned int)pid);
}