/* cmd_whoami.c — whoami command */
#include "shell_cmds.h"
#include "printf.h"
#include "process.h"

void cmd_whoami(void) {
    struct process *p = process_get_current();
    kprintf("PID %u (%s)\n", (uint64_t)p->pid, p->name);
}
