/* cmd_kill.c — kill command */
#include "shell_cmds.h"
#include "printf.h"
#include "process.h"
#include "signal.h"

void cmd_kill(const char *args) {
    if (!args) { kprintf("Usage: kill <pid> [signal]\n"); return; }
    uint32_t pid = 0;
    while (*args >= '0' && *args <= '9') { pid = pid * 10 + (*args - '0'); args++; }
    while (*args == ' ') args++;
    int sig = 9;
    if (*args >= '0' && *args <= '9') {
        sig = 0;
        while (*args >= '0' && *args <= '9') { sig = sig * 10 + (*args - '0'); args++; }
    }
    if (pid == 0) { kprintf("Cannot kill pid 0\n"); return; }
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) { kprintf("No such process: %u\n", (uint64_t)pid); return; }
    if (signal_send(pid, sig) < 0) {
        p->state = PROCESS_ZOMBIE;
    }
    kprintf("Signal %d sent to process %u (%s)\n", (uint64_t)sig, (uint64_t)pid, p->name);
}
