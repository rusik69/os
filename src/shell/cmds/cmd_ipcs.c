#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_ipcs(const char *args) {
    (void)args;
    kprintf("IPC status: Semaphores: 0, Message queues: 0, Shared memory: 0\n");
}
