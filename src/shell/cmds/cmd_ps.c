/* cmd_ps.c — ps command */
#include "shell_cmds.h"
#include "printf.h"
#include "process.h"

void cmd_ps(void) {
    extern struct process *process_get_table(void);
    struct process *table = process_get_table();
    const char *state_names[] = { "UNUSED", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };

    kprintf("PID  PPID STATE    MODE   BG NAME\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED) {
            kprintf("%-4u %-4u %-8s %-6s %-2s %s\n",
                    (uint64_t)table[i].pid,
                    (uint64_t)table[i].parent_pid,
                    state_names[table[i].state],
                    table[i].is_user ? "user" : "kernel",
                    table[i].is_background ? "&" : "",
                    table[i].name);
        }
    }
}
