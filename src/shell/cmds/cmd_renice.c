/* cmd_renice.c — change priority for an existing process */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

static int parse_uint_arg(const char **args, uint32_t *out) {
    const char *p = *args;
    while (*p == ' ') p++;
    if (!(*p >= '0' && *p <= '9')) return -1;
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (uint32_t)(*p - '0');
        p++;
    }
    *args = p;
    *out = value;
    return 0;
}

void cmd_renice(const char *args) {
    uint32_t pri = 0;
    uint32_t pid = 0;
    if (!args || parse_uint_arg(&args, &pri) != 0 || parse_uint_arg(&args, &pid) != 0) {
        kprintf("Usage: renice <priority> <pid>\n");
        return;
    }
    if (pri > 3) {
        kprintf("renice: priority must be 0-3\n");
        return;
    }
    if (libc_setpriority_pid(pid, (int)pri) != 0) {
        kprintf("renice: no such process: %u\n", (unsigned long)pid);
        return;
    }
    kprintf("%u priority set to %u\n", (unsigned long)pid, (unsigned long)pri);
}