/* cmd_meminfo.c — meminfo command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_meminfo(void) {
    struct pmm_stats stats = {0};
    pmm_get_stats(&stats);
    uint64_t total = stats.total_pages;
    uint64_t used = stats.used_pages;
    uint64_t free_fr = stats.free_pages;
    kprintf("Physical memory:\n");
    kprintf("  Total: %u KB (%u frames)\n", total * 4, total);
    kprintf("  Used:  %u KB (%u frames)\n", used * 4, used);
    kprintf("  Free:  %u KB (%u frames)\n", free_fr * 4, free_fr);
}
