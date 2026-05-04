/* cmd_fg.c — bring background process to foreground (wait for it) */
#include "shell_cmds.h"
#include "printf.h"
#include "process.h"

void cmd_fg(const char *args) {
    if (!args || !(*args >= '0' && *args <= '9')) {
        kprintf("Usage: fg <pid>\n");
        return;
    }
    uint32_t pid = 0;
    while (*args >= '0' && *args <= '9') { pid = pid * 10 + (*args - '0'); args++; }

    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) {
        kprintf("No such process: %u\n", (uint64_t)pid);
        return;
    }
    if (!p->is_background) {
        kprintf("Process %u is not a background job\n", (uint64_t)pid);
        return;
    }
    p->is_background = 0;
    int status = 0;
    process_waitpid(pid, &status);
    kprintf("[%u] Done\n", (uint64_t)pid);
}
