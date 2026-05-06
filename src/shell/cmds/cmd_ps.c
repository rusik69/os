/* cmd_ps.c — ps command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_ps(void) {
    struct libc_process_info procs[PROCESS_MAX];
    const char *state_names[] = { "UNUSED", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };
    int n = libc_process_list(procs, PROCESS_MAX);

    kprintf("PID  PPID STATE    MODE   BG NAME\n");
    for (int i = 0; i < n; i++) {
        uint8_t st = procs[i].state;
        if (st > 4) st = 0;
        kprintf("%-4u %-4u %-8s %-6s %-2s %s\n",
                (uint64_t)procs[i].pid,
                (uint64_t)procs[i].ppid,
                state_names[st],
                procs[i].is_user ? "user" : "kernel",
                procs[i].is_background ? "&" : "",
                procs[i].name);
    }
}
