/* cmd_fg.c — bring background process to foreground (wait for it) */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

static int parse_job_or_pid(const char *args, uint32_t *pid, uint32_t *pgid) {
    struct libc_process_info procs[PROCESS_MAX];
    if (!args) return -1;
    while (*args == ' ') args++;
    if (*args == '%') {
        args++;
        int job = 0;
        while (*args >= '0' && *args <= '9') { job = job * 10 + (*args - '0'); args++; }
        if (job <= 0) return -1;
        int n = libc_process_list(procs, PROCESS_MAX);
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
    int n = libc_process_list(procs, PROCESS_MAX);
    for (int i = 0; i < n; i++) {
        if (procs[i].pid == *pid) {
            *pgid = procs[i].pgid;
            return 0;
        }
    }
    *pgid = *pid;
    return 0;
}

void cmd_fg(const char *args) {
    uint32_t pid = 0;
    uint32_t pgid = 0;
    if (parse_job_or_pid(args, &pid, &pgid) != 0) {
        kprintf("Usage: fg <pid|%%job>\n");
        return;
    }

    if (pgid) libc_killpg(pgid, 18);
    else libc_kill(pid, 18);

    int status = 0;
    if (libc_waitpid(pid, &status) != 0) {
        kprintf("No such process: %u\n", (uint64_t)pid);
        return;
    }
    kprintf("[%u] Done\n", (uint64_t)pid);
}
