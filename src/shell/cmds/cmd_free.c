/* cmd_free.c — free command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_free(void) {
        struct pmm_stats stats = {0};
        pmm_get_stats(&stats);
        uint64_t total = stats.total_pages;
        uint64_t used = stats.used_pages;
        uint64_t free_fr = stats.free_pages;
        kprintf("              total      used       free\n");
        kprintf("Mem:     %9llu %9llu  %9llu  KB\n",
                (unsigned long long)(total * 4), (unsigned long long)(used * 4),
                (unsigned long long)(free_fr * 4));
        kprintf("Frames:  %9llu %9llu  %9llu\n",
                (unsigned long long)total, (unsigned long long)used,
                (unsigned long long)free_fr);
}
