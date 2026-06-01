/* cmd_chrt.c — real-time attributes */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

static const char *policy_name(int policy)
{
    switch (policy) {
        case 0:  return "SCHED_OTHER";
        case 1:  return "SCHED_FIFO";
        case 2:  return "SCHED_RR";
        default: return "SCHED_UNKNOWN";
    }
}

void cmd_chrt(const char *args)
{
    (void)args;
    kprintf("chrt: real-time attributes (stub)\n");
    kprintf("  current pid policy %s prio 0\n", policy_name(0));
}

void chrt_init(void)
{
    kprintf("[OK] cmd_chrt: real-time attributes command ready\n");
}
