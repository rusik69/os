/* cmd_bpftrace_probes.c — BPF probe management */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

void cmd_bpftrace_probes(const char *args)
{
    (void)args;
    kprintf("BPF trace probes:\n");
    kprintf("  kprobe:    available (kernel built-in)\n");
    kprintf("  uprobe:    available (kernel built-in)\n");
    kprintf("  tracepoint: available (kernel built-in)\n");
    kprintf("  Use 'bpftrace' command for trace scripts.\n");
}
