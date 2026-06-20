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
    kprintf("  Total: %llu KB (%llu frames)\n", (unsigned long long)(total * 4), (unsigned long long)total);
    kprintf("  Used:  %llu KB (%llu frames)\n", (unsigned long long)(used * 4), (unsigned long long)used);
    kprintf("  Free:  %llu KB (%llu frames)\n", (unsigned long long)(free_fr * 4), (unsigned long long)free_fr);
}
