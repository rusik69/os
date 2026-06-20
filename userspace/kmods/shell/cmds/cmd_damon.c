#include "shell_cmds.h"
#include "printf.h"

void cmd_damon(const char *args)
{
    (void)args;
    kprintf("DAMON (Data Access Monitoring):\n");
    kprintf("  Monitoring regions: 0\n");
    kprintf("  Total accesses:     0\n");
    kprintf("  Sampling interval:  5 ms\n");
    kprintf("  Aggregation interval: 100 ms\n");
    kprintf("  Min region size:    1 MB\n");
    kprintf("  Status:             idle\n");
}
