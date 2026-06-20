/* cmd_jobs.c — list background processes */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_jobs(void) {
    struct libc_process_info procs[PROCESS_MAX];
    const char *state_names[] = { "UNUSED", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };
    int found = 0;
    int n = libc_process_list(procs, PROCESS_MAX);

    kprintf("JOB  PID  PGID PRI STATE    NAME\n");
    for (int i = 0; i < n; i++) {
        if (procs[i].is_background) {
            uint8_t st = procs[i].state;
            if (st > 4) st = 0;
            const char *state = procs[i].is_suspended ? "STOPPED" : state_names[st];
            kprintf("%%%u  %-4u %-4u %-3u %-8s %s\n", (unsigned int)(found + 1),
                    (unsigned int)procs[i].pid,
                    (unsigned int)procs[i].pgid,
                    (unsigned int)procs[i].priority,
                    state, procs[i].name);
            found = 1;
        }
    }
    if (!found)
        kprintf("No background jobs\n");
}
