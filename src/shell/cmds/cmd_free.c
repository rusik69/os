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
    kprintf("Mem:     %9u %9u  %9u  KB\n",
            total * 4, used * 4, free_fr * 4);
    kprintf("Frames:  %9u %9u  %9u\n",
            total, used, free_fr);
}
