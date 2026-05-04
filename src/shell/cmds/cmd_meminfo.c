/* cmd_meminfo.c — meminfo command */
#include "shell_cmds.h"
#include "printf.h"
#include "pmm.h"

void cmd_meminfo(void) {
    uint64_t total = pmm_get_total_frames();
    uint64_t used = pmm_get_used_frames();
    uint64_t free_fr = total - used;
    kprintf("Physical memory:\n");
    kprintf("  Total: %u KB (%u frames)\n", total * 4, total);
    kprintf("  Used:  %u KB (%u frames)\n", used * 4, used);
    kprintf("  Free:  %u KB (%u frames)\n", free_fr * 4, free_fr);
}
