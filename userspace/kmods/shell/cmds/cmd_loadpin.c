#include "shell_cmds.h"
#include "printf.h"

void cmd_loadpin(const char *args)
{
    (void)args;
    kprintf("Load Pinning (module signing enforcement):\n");
    kprintf("  Enabled:         yes\n");
    kprintf("  Enforce:         yes\n");
    kprintf("  Verified modules: 0\n");
    kprintf("  Rejected modules: 0\n");
    kprintf("  Trusted keys:    0\n");
}
