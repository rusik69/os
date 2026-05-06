/* cmd_fg.c — bring background process to foreground (wait for it) */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_fg(const char *args) {
    if (!args || !(*args >= '0' && *args <= '9')) {
        kprintf("Usage: fg <pid>\n");
        return;
    }
    uint32_t pid = 0;
    while (*args >= '0' && *args <= '9') { pid = pid * 10 + (*args - '0'); args++; }

    int status = 0;
    if (libc_waitpid(pid, &status) != 0) {
        kprintf("No such process: %u\n", (uint64_t)pid);
        return;
    }
    kprintf("[%u] Done\n", (uint64_t)pid);
}
