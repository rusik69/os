#include "shell_cmds.h"
#include "printf.h"

void cmd_powercap(const char *args)
{
    (void)args;
    kprintf("Power Capping Information:\n");
    kprintf("  Package 0:\n");
    kprintf("    Power limit:    95 W\n");
    kprintf("    Time window:    1.0 s\n");
    kprintf("    Current power:  0 W\n");
    kprintf("  DRAM:\n");
    kprintf("    Power limit:    15 W\n");
    kprintf("    Current power:  0 W\n");
}
