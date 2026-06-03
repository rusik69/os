/* cmd_ps.c — ps command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_ps(void) {
    static struct libc_process_info procs[PROCESS_MAX];
    const char *state_names[] = { "UNUSED", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };
    int n = libc_process_list(procs, PROCESS_MAX);

    kprintf("PID  PPID PGID PRI STATE    MODE   BG NAME\n");
    for (int i = 0; i < n; i++) {
        uint8_t st = procs[i].state;
        if (st > 4) st = 0;
        const char *state = procs[i].is_suspended ? "STOPPED" : state_names[st];
        kprintf("%-4u %-4u %-4u %-3u %-8s %-6s %-2s %s\n",
                procs[i].pid,
                procs[i].ppid,
                procs[i].pgid,
                procs[i].priority,
                state,
                procs[i].is_user ? "user" : "kernel",
                procs[i].is_background ? "&" : "",
                procs[i].name);
    }
}
