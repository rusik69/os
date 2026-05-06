/* cmd_jobs.c — list background processes */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_jobs(void) {
    struct libc_process_info procs[PROCESS_MAX];
    const char *state_names[] = { "UNUSED", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };
    int found = 0;
    int n = libc_process_list(procs, PROCESS_MAX);

    kprintf("PID  STATE    NAME\n");
    for (int i = 0; i < n; i++) {
        if (procs[i].is_background) {
            uint8_t st = procs[i].state;
            if (st > 4) st = 0;
            kprintf("%-4u %-8s %s\n", (uint64_t)procs[i].pid,
                    state_names[st], procs[i].name);
            found = 1;
        }
    }
    if (!found)
        kprintf("No background jobs\n");
}
