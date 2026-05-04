/* cmd_free.c — free command */
#include "shell_cmds.h"
#include "printf.h"
#include "pmm.h"

void cmd_free(void) {
    uint64_t total = pmm_get_total_frames();
    uint64_t used = pmm_get_used_frames();
    uint64_t free_fr = total - used;
    kprintf("              total      used       free\n");
    kprintf("Mem:     %9u %9u  %9u  KB\n",
            total * 4, used * 4, free_fr * 4);
    kprintf("Frames:  %9u %9u  %9u\n",
            total, used, free_fr);
}
