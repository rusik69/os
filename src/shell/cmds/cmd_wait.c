/* cmd_wait.c — wait for a background process to finish */
#include "shell_cmds.h"
#include "printf.h"
#include "process.h"

void cmd_wait(const char *args) {
    if (!args || !(*args >= '0' && *args <= '9')) {
        kprintf("Usage: wait <pid>\n");
        return;
    }
    uint32_t pid = 0;
    while (*args >= '0' && *args <= '9') { pid = pid * 10 + (*args - '0'); args++; }

    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) {
        kprintf("No such process: %u\n", (uint64_t)pid);
        return;
    }
    int status = 0;
    if (process_waitpid(pid, &status) == 0) {
        kprintf("[%u] Exited with status %d\n", (uint64_t)pid, (uint64_t)status);
    } else {
        kprintf("Failed to wait for process %u\n", (uint64_t)pid);
    }
}
