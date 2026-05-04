/* cmd_jobs.c — list background processes */
#include "shell_cmds.h"
#include "printf.h"
#include "process.h"

void cmd_jobs(void) {
    struct process *table = process_get_table();
    const char *state_names[] = { "UNUSED", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };
    int found = 0;

    kprintf("PID  STATE    NAME\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED && table[i].is_background) {
            kprintf("%-4u %-8s %s\n", (uint64_t)table[i].pid,
                    state_names[table[i].state], table[i].name);
            found = 1;
        }
    }
    if (!found)
        kprintf("No background jobs\n");
}
