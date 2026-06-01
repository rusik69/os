/* cmd_vmstat.c — Show virtual memory statistics */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_vmstat(const char *args) {
    (void)args;
    struct pmm_stats stats = {0};
    pmm_get_stats(&stats);

    uint32_t total_kb = stats.total_pages * 4;
    uint32_t used_kb  = stats.used_pages * 4;
    uint32_t free_kb  = stats.free_pages * 4;

    kprintf("Virtual Memory Statistics:\n");
    kprintf("-------------------------\n");
    kprintf("Total RAM:      %u KB (%u pages)\n", (unsigned long)total_kb, (unsigned long)stats.total_pages);
    kprintf("Used RAM:       %u KB (%u pages)\n", (unsigned long)used_kb, (unsigned long)stats.used_pages);
    kprintf("Free RAM:       %u KB (%u pages)\n", (unsigned long)free_kb, (unsigned long)stats.free_pages);
    kprintf("\n");
    kprintf("Uptime ticks:   %u\n", (unsigned long)libc_uptime_ticks());
    kprintf("Active PIDs:    %u\n", (unsigned long)libc_getpid());
}
