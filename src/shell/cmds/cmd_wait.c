/* cmd_wait.c — wait command: wait for a process to finish */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_wait(const char *args) {
    if (!args || !*args) { kprintf("Usage: wait <pid>\n"); return; }
    uint32_t pid = 0;
    while (*args >= '0' && *args <= '9') { pid = pid * 10 + (*args - '0'); args++; }

    int status = 0;
    if (libc_waitpid(pid, &status) == 0) {
        kprintf("[%u] Exited with status %d\n", pid, status);
    } else {
        kprintf("Failed to wait for process %u\n", pid);
    }
}
