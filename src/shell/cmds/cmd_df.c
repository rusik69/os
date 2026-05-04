/* cmd_df.c — df command */
#include "shell_cmds.h"
#include "printf.h"
#include "fs.h"
#include "ata.h"

void cmd_df(void) {
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    uint32_t total_sectors = ata_get_sectors();
    uint32_t used_inodes, total_inodes, used_blocks, data_start;
    fs_get_usage(&used_inodes, &total_inodes, &used_blocks, &data_start);
    uint32_t avail = total_sectors > (data_start + used_blocks)
                     ? total_sectors - data_start - used_blocks : 0;
    kprintf("Filesystem      Blocks  Used    Avail   Inodes\n");
    kprintf("/dev/hda        %-7u %-7u %-7u %u/%u\n",
            (uint64_t)(total_sectors - data_start),
            (uint64_t)used_blocks,
            (uint64_t)avail,
            (uint64_t)used_inodes,
            (uint64_t)total_inodes);
}
