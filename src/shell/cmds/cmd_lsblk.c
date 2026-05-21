#include "shell_cmds.h"
#include "blockdev.h"
#include "printf.h"

void cmd_lsblk(void) {
    kprintf("NAME     SIZE    TYPE\n");
    int any = 0;
    for (int id = 0; id < BLOCKDEV_MAX_DEVICES; id++) {
        if (!blockdev_is_registered(id)) continue;
        any = 1;
        uint32_t sects = blockdev_get_sectors(id);
        uint32_t mb = sects / 2048;
        kprintf("%-8s %u MB   disk\n", blockdev_name(id), (uint64_t)mb);
    }
    if (!any)
        kprintf("(no block devices detected)\n");
}
